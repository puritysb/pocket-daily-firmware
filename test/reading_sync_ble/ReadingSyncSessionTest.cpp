// READ_LIST / OFFER / W end to end: the real record half of the Nearby Sync
// service, ReadingSyncSession, the exchange shared with the HTTP routes
// (ReadingExchange) and the reading-progress store on the fake SD card. Only
// the NimBLE half is faked (FakeRadio.cpp).
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Epub.h>
#include <HalStorage.h>
#include <KOReaderDocumentId.h>
#include <RecentBooksStore.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "FakeRadio.h"
#include "pocket_daily/ReadingExchange.h"
#include "pocket_daily/ReadingProgress.h"
#include "pocket_daily/ReadingProgressStore.h"
#include "pocket_daily/nearby_sync/NearbySyncService.h"
#include "pocket_daily/nearby_sync/ReadingSyncProtocol.h"
#include "pocket_daily/nearby_sync/ReadingSyncSession.h"

namespace Reading = PocketDaily::ReadingProgress;
using namespace Pocket::NearbySync;

namespace {
constexpr char READER_ID[] = "89ABCDEF";  // FakeRadio's Service::begin
constexpr char BOOK_A[] = "/Books/Frankenstein.epub";
constexpr char NOTES[] = "/Books/notes.txt";
constexpr char BOOK_C[] = "/Books/한글 책.epub";
constexpr char DIGEST_A[] = "0123456789abcdef0123456789abcdef";
constexpr char FILENAME_A[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr char DIGEST_C[] = "fedcba9876543210fedcba9876543210";
constexpr char FILENAME_C[] = "cccccccccccccccccccccccccccccccc";
constexpr char XPOINTER_A[] = "/body/DocFragment[3]/body/p[4]/text()[1].12";

std::string cacheOf(const char* path) { return Epub(path, "/.crosspoint").getCachePath(); }

uint32_t crcOf(const std::string& text) { return crcFinish(crcUpdate(CRC_START, text.data(), text.size())); }

std::string hex8(const uint32_t value) {
  char out[9];
  snprintf(out, sizeof(out), "%08lX", static_cast<unsigned long>(value));
  return out;
}

void seedLibrary() {
  FakeSD::reset();
  ReadingSyncTestHost::freeHeap = 100000;
  ReadingSyncTestHost::largestBlock = 60000;
  ReadingSyncTestHost::contentDigests = {{BOOK_A, DIGEST_A}, {BOOK_C, DIGEST_C}};
  ReadingSyncTestHost::filenameDigests = {{BOOK_A, FILENAME_A}, {BOOK_C, FILENAME_C}};
  FakeSD::files[BOOK_A] = std::vector<uint8_t>(2048, 'a');
  FakeSD::files[NOTES] = std::vector<uint8_t>(10, 'n');
  FakeSD::files[BOOK_C] = std::vector<uint8_t>(4096, 'c');
  RECENT_BOOKS.books = {{BOOK_A, "", "", ""}, {NOTES, "", "", ""}, {BOOK_C, "", "", ""}};

  // Book A was left on spine 2, page 5 of 10 with an XPointer; C has no record.
  Reading::Record record;
  record.spine = 2;
  record.page = 5;
  record.pageCount = 10;
  record.percentage = 0.25f;
  record.seq = 3;
  record.updated = 1790000000;
  record.fileSize = 2048;
  strcpy(record.document, DIGEST_A);
  strcpy(record.xpointer, XPOINTER_A);
  Reading::Scratch scratch;
  ASSERT_TRUE(Reading::saveRecord(cacheOf(BOOK_A).c_str(), record, scratch));
  FakeSD::files[cacheOf(BOOK_A) + "/progress.bin"] = {2, 0, 5, 0, 10, 0, 25};
  FakeSD::files[cacheOf(BOOK_C) + "/progress.bin"] = {1, 0, 0, 0, 8, 0, 40};
}

// The owning loop of an exchange window, minus the radio: drain the queued
// records into the session, then one pump (at most one notification).
struct Link {
  Service service;
  ReadingSyncSession session{service};

  Link() {
    FakeRadio::reset();
    service.begin("X3", "test", Mode::WINDOW, 45000);
    service.onConnected(true, true, 1, 0);
  }
  void write(const std::string& record) { service.onCommandRecord(record.data(), record.size()); }
  void pass() {
    ParsedCommand command;
    while (service.takeCommand(command)) session.handle(command);
    session.pump();
  }
  void settle() {
    pass();
    for (int i = 0; i < 2000 && session.busy(); ++i) pass();
  }
};

struct ListResult {
  std::string body;
  std::vector<std::string> records;
  std::string end;
};

ListResult readList(Link& link, const std::string& id) {
  ListResult result;
  const size_t before = FakeRadio::sent.size();
  link.write("READ_LIST " + id);
  for (int i = 0; i < 2000; ++i) {
    const size_t sentBefore = FakeRadio::sent.size();
    link.pass();
    EXPECT_LE(FakeRadio::sent.size(), sentBefore + 1) << "one notification per pass";
    if (!link.session.busy()) break;
  }
  for (size_t i = before; i < FakeRadio::sent.size(); ++i) {
    const std::string& record = FakeRadio::sent[i];
    EXPECT_LE(record.size(), RECORD_LIMIT);
    result.records.push_back(record);
    if (record.rfind("D " + id + " ", 0) == 0) {
      const size_t seqEnd = record.find(' ', 11);
      const uint32_t seq = static_cast<uint32_t>(std::stoul(record.substr(11, seqEnd - 11)));
      EXPECT_EQ(seq, result.records.size() - 1) << "chunks in order from 0";
      const std::string chunk = record.substr(seqEnd + 1);
      EXPECT_LE(chunk.size(), MAX_DATA_CHUNK);
      result.body += chunk;
    } else {
      result.end = record;
    }
  }
  return result;
}

std::string listBody(const bool includePaths) {
  auto work = std::make_unique<Reading::ExchangeWork>();
  Reading::ListStream stream(READER_ID, includePaths, *work);
  std::string body;
  for (size_t length; (length = stream.next(work->entry, sizeof(work->entry))) != 0;) body.append(work->entry, length);
  return body;
}

// Sends an OFFER in W chunks of `chunkSize` bytes; returns the replies.
std::vector<std::string> offer(Link& link, const std::string& id, const std::string& body, const size_t chunkSize,
                               const uint32_t crc) {
  const size_t before = FakeRadio::sent.size();
  link.write("OFFER " + id + " " + std::to_string(body.size()) + " " + hex8(crc));
  link.pass();
  uint32_t seq = 0;
  for (size_t offset = 0; offset < body.size(); offset += chunkSize) {
    link.write("W " + id + " " + std::to_string(seq++) + " " + body.substr(offset, chunkSize));
    link.pass();  // write with response: the app sends the next chunk after the reply
  }
  link.settle();
  return {FakeRadio::sent.begin() + static_cast<long>(before), FakeRadio::sent.end()};
}

std::string appOfferBody(const char* document, const char* device = "민수의 iPhone", const char* deviceId = READER_ID,
                         const char* percentage = "0.52") {
  // The app's HTTP body: sorted keys, unescaped slashes.
  return std::string("{\"device\":\"") + device + "\",\"deviceID\":\"" + deviceId + "\",\"document\":\"" + document +
         "\",\"percentage\":" + percentage + ",\"progress\":\"/body/DocFragment[3]/body/p[1]/text()[1].14061\"}";
}
}  // namespace

TEST(ReadingSyncSession, ListIsTheHttpBodyWithoutPaths) {
  seedLibrary();
  Link link;
  const ListResult list = readList(link, "0000001A");
  ASSERT_FALSE(list.body.empty());
  EXPECT_EQ(list.body.find("\"path\""), std::string::npos);
  EXPECT_EQ(list.body.find("Frankenstein"), std::string::npos) << "no file names cross the air";
  EXPECT_EQ(list.body, listBody(false));
  EXPECT_EQ(list.end, "END 0000001A " + std::to_string(list.body.size()) + " " + hex8(crcOf(list.body)));

  JsonDocument ble;
  ASSERT_FALSE(deserializeJson(ble, list.body)) << list.body;
  JsonDocument http;
  const std::string httpBody = listBody(true);
  ASSERT_FALSE(deserializeJson(http, httpBody)) << httpBody;
  ASSERT_EQ(http["books"].size(), 2u);  // the TXT is not listed
  EXPECT_STREQ(http["books"][0]["path"], BOOK_A);
  for (JsonObject book : http["books"].as<JsonArray>()) book.remove("path");
  std::string httpWithoutPaths;
  std::string bleCanonical;
  serializeJson(http, httpWithoutPaths);
  serializeJson(ble, bleCanonical);
  EXPECT_EQ(bleCanonical, httpWithoutPaths);

  EXPECT_STREQ(ble["deviceID"], READER_ID);
  EXPECT_STREQ(ble["books"][0]["document"], DIGEST_A);
  EXPECT_STREQ(ble["books"][0]["filenameDocument"], FILENAME_A);
  EXPECT_STREQ(ble["books"][0]["progress"], XPOINTER_A);
  EXPECT_NEAR(ble["books"][0]["percentage"].as<double>(), 0.25, 1e-5);
  EXPECT_TRUE(ble["books"][1]["progress"].isNull());
  EXPECT_NEAR(ble["books"][1]["percentage"].as<double>(), 0.40, 1e-5);
  EXPECT_FALSE(link.session.busy());
  EXPECT_EQ(link.session.listsSent(), 1u);
}

TEST(ReadingSyncSession, NotificationsTheStackRefusesAreRetried) {
  seedLibrary();
  Link link;
  link.write("READ_LIST 0000001B");
  link.pass();
  link.pass();
  FakeRadio::acceptNotifications = false;
  for (int i = 0; i < 20; ++i) link.pass();
  EXPECT_GT(FakeRadio::rejectedNotifications, 0);
  FakeRadio::acceptNotifications = true;
  link.settle();
  std::string body;
  std::string end;
  uint32_t expectedSeq = 0;
  for (const auto& record : FakeRadio::sent) {
    if (record.rfind("D 0000001B ", 0) == 0) {
      const size_t seqEnd = record.find(' ', 11);
      EXPECT_EQ(std::stoul(record.substr(11, seqEnd - 11)), expectedSeq++);
      body += record.substr(seqEnd + 1);
    } else {
      end = record;
    }
  }
  EXPECT_EQ(body, listBody(false));
  EXPECT_EQ(end, "END 0000001B " + std::to_string(body.size()) + " " + hex8(crcOf(body)));
  JsonDocument doc;
  EXPECT_FALSE(deserializeJson(doc, body));
}

TEST(ReadingSyncSession, OfferIsValidatedAndStoredLikeHttp) {
  seedLibrary();
  Link link;
  const std::string body = appOfferBody(DIGEST_A);
  // 7-byte chunks split the Korean device name mid-character and start some
  // chunks with a space.
  const auto replies = offer(link, "0000002A", body, 7, crcOf(body));
  ASSERT_EQ(replies, std::vector<std::string>{"OK 0000002A"});
  Reading::Offer stored;
  Reading::Scratch scratch;
  ASSERT_TRUE(Reading::loadOffer(cacheOf(BOOK_A).c_str(), stored, scratch));
  EXPECT_STREQ(stored.device, "민수의 iPhone");
  EXPECT_STREQ(stored.xpointer, "/body/DocFragment[3]/body/p[1]/text()[1].14061");
  EXPECT_NEAR(stored.percentage, 0.52, 1e-6);
  // The current position is never touched.
  Reading::Record record;
  ASSERT_TRUE(Reading::loadRecord(cacheOf(BOOK_A).c_str(), record, scratch));
  EXPECT_STREQ(record.xpointer, XPOINTER_A);
  EXPECT_EQ(link.session.offersStored(), 1u);

  // KOReader filename mode matches too; upper-case digests are accepted.
  const std::string byName = appOfferBody("CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC", "Pocket Daily iPad");
  EXPECT_EQ(offer(link, "0000002B", byName, 180, crcOf(byName)), std::vector<std::string>{"OK 0000002B"});
  ASSERT_TRUE(Reading::loadOffer(cacheOf(BOOK_C).c_str(), stored, scratch));
  EXPECT_STREQ(stored.device, "Pocket Daily iPad");

  // Any key order, as over HTTP.
  const std::string reordered = std::string("{\"percentage\":0.6,\"progress\":\"/body/DocFragment[2]/body\",") +
                                "\"deviceID\":\"" + READER_ID + "\",\"device\":\"Mac\",\"document\":\"" + DIGEST_A +
                                "\",\"extra\":1}";
  EXPECT_EQ(offer(link, "0000002C", reordered, 50, crcOf(reordered)), std::vector<std::string>{"OK 0000002C"});
}

TEST(ReadingSyncSession, OfferErrors) {
  seedLibrary();
  Link link;
  const std::string unknown = appOfferBody("11111111111111111111111111111111");
  EXPECT_EQ(offer(link, "0000003A", unknown, 180, crcOf(unknown)),
            std::vector<std::string>{"ERR 0000003A UNKNOWN_DOCUMENT"});
  const std::string badPercentage = appOfferBody(DIGEST_A, "Phone", READER_ID, "1.5");
  EXPECT_EQ(offer(link, "0000003B", badPercentage, 180, crcOf(badPercentage)),
            std::vector<std::string>{"ERR 0000003B BAD_RECORD"});
  const std::string otherReader = appOfferBody(DIGEST_A, "Phone", "00000000");
  EXPECT_EQ(offer(link, "0000003C", otherReader, 180, crcOf(otherReader)),
            std::vector<std::string>{"ERR 0000003C BAD_RECORD"});
  // A CRC mismatch is reported once, not for every chunk still in flight.
  const std::string good = appOfferBody(DIGEST_A);
  EXPECT_EQ(offer(link, "0000003D", good, 40, crcOf(good) ^ 1u), std::vector<std::string>{"ERR 0000003D BAD_CHUNK"});
  EXPECT_FALSE(FakeSD::files.count(cacheOf(BOOK_A) + Reading::OFFER_FILE));

  size_t before = FakeRadio::sent.size();
  link.write("OFFER 0000003E 1025 00000000");
  link.write("OFFER 0000003F 0 00000000");
  link.write("W 00000040 0 {}");  // no offer announced
  link.write("OFFER 00000041 12");
  link.settle();
  // Parse errors are answered at once, session errors on the next passes.
  std::vector<std::string> replies(FakeRadio::sent.begin() + static_cast<long>(before), FakeRadio::sent.end());
  std::sort(replies.begin(), replies.end());
  EXPECT_EQ(replies, (std::vector<std::string>{"ERR 0000003E BAD_RECORD", "ERR 0000003F BAD_RECORD",
                                               "ERR 00000040 BAD_CHUNK", "ERR 00000041 BAD_RECORD"}));

  // Out-of-order chunk.
  before = FakeRadio::sent.size();
  link.write("OFFER 00000042 " + std::to_string(good.size()) + " " + hex8(crcOf(good)));
  link.write("W 00000042 1 " + good.substr(0, 20));
  link.settle();
  EXPECT_EQ(FakeRadio::sent.back(), "ERR 00000042 BAD_CHUNK");

  // Low memory is an answer, not a crash.
  ReadingSyncTestHost::freeHeap = 8000;
  link.write("OFFER 00000043 " + std::to_string(good.size()) + " " + hex8(crcOf(good)));
  link.write("READ_LIST 00000044");
  link.settle();
  EXPECT_EQ(FakeRadio::sent[FakeRadio::sent.size() - 2], "ERR 00000043 NO_MEMORY");
  EXPECT_EQ(FakeRadio::sent.back(), "ERR 00000044 NO_MEMORY");
}

TEST(ReadingSyncSession, OneExchangeAtATimeAndNothingOutlivesTheLink) {
  seedLibrary();
  Link link;
  const std::string good = appOfferBody(DIGEST_A);
  // An OFFER while the list streams is BUSY; the list still completes.
  link.write("READ_LIST 0000005A");
  link.pass();
  link.write("OFFER 0000005B " + std::to_string(good.size()) + " " + hex8(crcOf(good)));
  link.settle();
  EXPECT_NE(std::find(FakeRadio::sent.begin(), FakeRadio::sent.end(), "ERR 0000005B BUSY"), FakeRadio::sent.end());
  EXPECT_EQ(FakeRadio::sent.back().rfind("END 0000005A ", 0), 0u);

  // A disconnect discards a half-sent offer; its chunks on the next link are
  // answered as unknown.
  link.write("OFFER 0000005C " + std::to_string(good.size()) + " " + hex8(crcOf(good)));
  link.write("W 0000005C 0 " + good.substr(0, 30));
  link.pass();
  EXPECT_TRUE(link.session.busy());
  link.service.onConnected(false, false, 1, 100);
  link.service.onConnected(true, true, 2, 200);
  link.pass();
  EXPECT_FALSE(link.session.busy());
  link.write("W 0000005C 1 " + good.substr(30, 30));
  link.settle();
  EXPECT_EQ(FakeRadio::sent.back(), "ERR 0000005C BAD_CHUNK");
  EXPECT_FALSE(FakeSD::files.count(cacheOf(BOOK_A) + Reading::OFFER_FILE));

  // Records queued on a link that dropped are never served on the next one.
  const size_t before = FakeRadio::sent.size();
  link.write("READ_LIST 0000005D");
  link.service.onConnected(false, false, 2, 300);
  link.service.onConnected(true, true, 3, 400);
  link.settle();
  EXPECT_EQ(FakeRadio::sent.size(), before);
}

TEST(ReadingSyncSession, QueueOverflowFailsTheOfferWithBusy) {
  seedLibrary();
  Link link;
  const std::string good = appOfferBody(DIGEST_A);
  link.write("OFFER 0000006A " + std::to_string(good.size()) + " " + hex8(crcOf(good)));
  link.pass();
  // Five chunks arrive before the loop runs: the fifth is dropped.
  for (int seq = 0; seq < 5; ++seq) {
    link.write("W 0000006A " + std::to_string(seq) + " " + good.substr(static_cast<size_t>(seq) * 20, 20));
  }
  link.settle();
  EXPECT_EQ(FakeRadio::sent.back(), "ERR 0000006A BUSY");
  EXPECT_FALSE(link.session.busy());
}

TEST(ReadingSyncSession, MalformedAndUnknownCommandsAreAnswered) {
  seedLibrary();
  Link link;
  link.write("READ_LIST 0000007a");
  link.write("SHUTDOWN 0000007B");
  link.write("READ_LIST 0000007C extra");
  link.settle();
  EXPECT_EQ(FakeRadio::sent, (std::vector<std::string>{"ERR 00000000 BAD_COMMAND", "ERR 0000007B UNKNOWN_COMMAND",
                                                       "ERR 0000007C BAD_COMMAND"}));
}

TEST(ReadingSyncSession, HeapLossBetweenOfferAndFinalChunkDoesNotPersist) {
  seedLibrary();
  Link link;
  const std::string body = appOfferBody(DIGEST_A);
  link.write("OFFER 000000AA " + std::to_string(body.size()) + " " + hex8(crcOf(body)));
  link.pass();
  ASSERT_TRUE(link.session.busy());
  ReadingSyncTestHost::largestBlock = 2048;
  size_t offset = 0;
  unsigned seq = 0;
  while (offset < body.size()) {
    const auto chunk = body.substr(offset, 180);
    link.write("W 000000AA " + std::to_string(seq++) + " " + chunk);
    link.pass();
    offset += chunk.size();
  }
  link.settle();
  ASSERT_FALSE(FakeRadio::sent.empty());
  EXPECT_EQ(FakeRadio::sent.back(), "ERR 000000AA NO_MEMORY");
  EXPECT_FALSE(FakeSD::files.count(cacheOf(BOOK_A) + Reading::OFFER_FILE));
  EXPECT_EQ(link.session.offersStored(), 0);
}

TEST(ReadingSyncService, CountsConnectionsIncludingStartupWithoutCountingDisconnects) {
  Service service;
  ASSERT_TRUE(service.begin("X4", "test", Mode::WINDOW));
  EXPECT_EQ(service.connectionCount(), 0u);
  service.onConnected(true, false, 1, 100);
  EXPECT_EQ(service.connectionCount(), 1u);
  service.onAuthenticated(true);
  service.onConnected(false, false, 1, 200);
  EXPECT_EQ(service.connectionCount(), 1u);
  service.onConnected(true, true, 2, 300);
  EXPECT_EQ(service.connectionCount(), 2u);
  EXPECT_EQ(service.connectionGeneration(), 3u);
  service.end();
}

TEST(ReadingSyncSession, CausalOfferRejectsChangedReaderAndStoresMatchingObservation) {
  seedLibrary();
  Link link;
  auto body = appOfferBody(DIGEST_A, "Phone", READER_ID, "0.1");
  body.pop_back();
  body += ",\"readerSeq\":2}";
  EXPECT_EQ(offer(link, "00000090", body, 29, crcOf(body)), std::vector<std::string>{"ERR 00000090 STALE_POSITION"});
  EXPECT_FALSE(Reading::hasOffer(cacheOf(BOOK_A).c_str()));
  body.replace(body.rfind(":2}"), 3, ":3}");
  EXPECT_EQ(offer(link, "00000091", body, 31, crcOf(body)), std::vector<std::string>{"OK 00000091"});
  Reading::Offer saved;
  Reading::Scratch scratch;
  ASSERT_TRUE(Reading::loadOffer(cacheOf(BOOK_A).c_str(), saved, scratch));
  EXPECT_EQ(saved.readerSeq, 3u);
  EXPECT_TRUE(Reading::shouldSuggest(saved, 3, 0.25f));
  EXPECT_FALSE(Reading::shouldSuggest(saved, 4, 0.25f));
}
