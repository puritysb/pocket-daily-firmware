// Reading-progress v1 records, offers and JSON (docs/reading-progress-v1.md).
#include <ArduinoJson.h>
#include <HalStorage.h>
#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "pocket_daily/ReadingProgress.h"
#include "pocket_daily/ReadingProgressStore.h"

namespace Reading = PocketDaily::ReadingProgress;

namespace {
constexpr char DIGEST[] = "0123456789abcdef0123456789abcdef";
constexpr char CACHE[] = "/.crosspoint/epub_42";

Reading::Record sampleRecord() {
  Reading::Record record;
  record.spine = 3;
  record.page = 17;
  record.pageCount = 40;
  record.percentage = 0.4321f;
  record.seq = 9;
  record.updated = 1790000000;
  record.fileSize = 123456;
  strcpy(record.document, DIGEST);
  strcpy(record.xpointer, "/body/DocFragment[4]/body/section[1]/p[12]/text()[1].96");
  return record;
}

Reading::Offer sampleOffer() {
  Reading::Offer offer;
  offer.percentage = 0.5f;
  strcpy(offer.xpointer, "/body/DocFragment[3]/body/p[12]/text().0");
  strcpy(offer.device, "Pocket Daily iPhone");
  return offer;
}

std::string offerBody(const char* deviceID, const char* document, const char* progress, const char* percentage,
                      const char* device) {
  return std::string("{\"deviceID\":") + deviceID + ",\"document\":" + document + ",\"progress\":" + progress +
         ",\"percentage\":" + percentage + ",\"device\":" + device + "}";
}

bool parse(const std::string& body, Reading::OfferRequest& out, std::string* errorText = nullptr) {
  const char* error = nullptr;
  bool oom = false;
  const bool ok = Reading::parseOfferJson(body.c_str(), body.size(), out, error, oom);
  if (errorText && error) *errorText = error;
  EXPECT_FALSE(oom);
  return ok;
}
}  // namespace

TEST(ReadingProgressFormat, RecordRoundTripsAndRejectsDamage) {
  uint8_t bytes[Reading::MAX_RECORD_BYTES];
  const auto record = sampleRecord();
  const size_t size = Reading::encodeRecord(record, bytes, sizeof(bytes));
  ASSERT_GT(size, 0u);
  Reading::Record decoded;
  ASSERT_TRUE(Reading::decodeRecord(bytes, size, decoded));
  EXPECT_EQ(decoded.spine, 3);
  EXPECT_EQ(decoded.page, 17);
  EXPECT_EQ(decoded.pageCount, 40);
  EXPECT_FLOAT_EQ(decoded.percentage, 0.4321f);
  EXPECT_EQ(decoded.seq, 9u);
  EXPECT_EQ(decoded.updated, 1790000000u);
  EXPECT_EQ(decoded.fileSize, 123456u);
  EXPECT_STREQ(decoded.document, DIGEST);
  EXPECT_STREQ(decoded.xpointer, record.xpointer);

  for (size_t i = 0; i < size; ++i) {
    uint8_t damaged[Reading::MAX_RECORD_BYTES];
    memcpy(damaged, bytes, size);
    damaged[i] ^= 0x40;
    EXPECT_FALSE(Reading::decodeRecord(damaged, size, decoded)) << "byte " << i;
    EXPECT_EQ(decoded.xpointer[0], '\0');
  }
  EXPECT_FALSE(Reading::decodeRecord(bytes, size - 1, decoded));
  EXPECT_FALSE(Reading::decodeRecord(bytes, 3, decoded));
  EXPECT_FALSE(Reading::decodeRecord(nullptr, 0, decoded));
  EXPECT_EQ(Reading::encodeRecord(record, bytes, size - 1), 0u);
}

TEST(ReadingProgressFormat, RecordWithoutXPointerOrDigestIsValid) {
  Reading::Record record = sampleRecord();
  record.xpointer[0] = '\0';
  record.document[0] = '\0';
  uint8_t bytes[Reading::MAX_RECORD_BYTES];
  const size_t size = Reading::encodeRecord(record, bytes, sizeof(bytes));
  ASSERT_GT(size, 0u);
  Reading::Record decoded;
  ASSERT_TRUE(Reading::decodeRecord(bytes, size, decoded));
  EXPECT_EQ(decoded.xpointer[0], '\0');
  EXPECT_EQ(decoded.document[0], '\0');

  record.percentage = 1.5f;
  EXPECT_EQ(Reading::encodeRecord(record, bytes, sizeof(bytes)), 0u);
  record = sampleRecord();
  strcpy(record.xpointer, "/body/p[1]");
  EXPECT_EQ(Reading::encodeRecord(record, bytes, sizeof(bytes)), 0u);
  record = sampleRecord();
  memset(record.xpointer, 'x', Reading::MAX_XPOINTER_BYTES);
  memcpy(record.xpointer, "/body/DocFragment[", 18);
  EXPECT_EQ(Reading::encodeRecord(record, bytes, sizeof(bytes)), Reading::MAX_RECORD_BYTES);  // 512 bytes fit
}

TEST(ReadingProgressFormat, OfferRoundTripsAndValidatesFields) {
  uint8_t bytes[Reading::MAX_OFFER_BYTES];
  const auto offer = sampleOffer();
  const size_t size = Reading::encodeOffer(offer, bytes, sizeof(bytes));
  ASSERT_GT(size, 0u);
  Reading::Offer decoded;
  ASSERT_TRUE(Reading::decodeOffer(bytes, size, decoded));
  EXPECT_FLOAT_EQ(decoded.percentage, 0.5f);
  EXPECT_STREQ(decoded.xpointer, offer.xpointer);
  EXPECT_STREQ(decoded.device, offer.device);
  bytes[size - 1] ^= 1;
  EXPECT_FALSE(Reading::decodeOffer(bytes, size, decoded));

  Reading::Offer invalid = offer;
  invalid.device[0] = '\0';
  EXPECT_EQ(Reading::encodeOffer(invalid, bytes, sizeof(bytes)), 0u);
  invalid = offer;
  invalid.xpointer[0] = '\0';
  EXPECT_EQ(Reading::encodeOffer(invalid, bytes, sizeof(bytes)), 0u);

  // The largest offer (512-byte XPointer, 64-byte device name) fits the buffer exactly.
  Reading::Offer largest = offer;
  memset(largest.xpointer, '1', Reading::MAX_XPOINTER_BYTES);
  memcpy(largest.xpointer, "/body/DocFragment[", 18);
  memset(largest.device, 'd', Reading::MAX_DEVICE_BYTES);
  EXPECT_EQ(Reading::encodeOffer(largest, bytes, sizeof(bytes)), Reading::MAX_OFFER_BYTES);
}

TEST(ReadingProgressFormat, OfferJsonAcceptsTheAppBody) {
  Reading::OfferRequest request;
  ASSERT_TRUE(parse(offerBody("\"ABCD1234\"", "\"0123456789ABCDEF0123456789abcdef\"",
                              "\"/body/DocFragment[3]/body/p[12]/text().0\"", "0.5", "\"Pocket Daily iPhone\""),
                    request));
  EXPECT_STREQ(request.deviceID, "ABCD1234");
  EXPECT_STREQ(request.document, "0123456789abcdef0123456789abcdef");  // digest compared lower-case
  EXPECT_STREQ(request.offer.xpointer, "/body/DocFragment[3]/body/p[12]/text().0");
  EXPECT_FLOAT_EQ(request.offer.percentage, 0.5f);
  EXPECT_STREQ(request.offer.device, "Pocket Daily iPhone");
  // Integer percentages and extra keys are fine; Korean device names are UTF-8.
  EXPECT_TRUE(
      parse("{\"deviceID\":\"ABCD1234\",\"document\":\"" + std::string(DIGEST) +
                "\",\"progress\":\"/body/DocFragment[1]/body\",\"percentage\":1,\"device\":\"아이패드\",\"x\":0}",
            request));
}

TEST(ReadingProgressFormat, OfferJsonRejectsMalformedFields) {
  Reading::OfferRequest request;
  const std::string good[] = {"\"ABCD1234\"", "\"" + std::string(DIGEST) + "\"",
                              "\"/body/DocFragment[3]/body/p[12]/text().0\"", "0.5", "\"iPhone\""};
  const std::vector<std::pair<int, std::string>> bad = {
      {0, "\"ABCD123\""},
      {0, "12345678"},
      {1, "\"0123\""},
      {1, "\"0123456789abcdef0123456789abcdeg\""},
      {2, "\"/body/p[1]/text().0\""},
      {2, "\"/body/DocFragment[1]/body/p[1]/text().0 \""},
      {2, "\"/body/DocFragment[" + std::string(500, '1') + "]\""},
      {2, "null"},
      {3, "1.01"},
      {3, "-0.1"},
      {3, "\"0.5\""},
      {4, "\"\""},
      {4, "\"" + std::string(65, 'a') + "\""},
      {4, "\"bad\\u0001name\""},
  };
  for (const auto& [field, value] : bad) {
    std::string parts[5] = {good[0], good[1], good[2], good[3], good[4]};
    parts[field] = value;
    EXPECT_FALSE(parse(
        offerBody(parts[0].c_str(), parts[1].c_str(), parts[2].c_str(), parts[3].c_str(), parts[4].c_str()), request))
        << field << " " << value;
    EXPECT_EQ(request.offer.xpointer[0], '\0');
  }
  EXPECT_FALSE(parse("[]", request));
  EXPECT_FALSE(parse("{", request));
  EXPECT_FALSE(parse(std::string(Reading::MAX_OFFER_BODY_BYTES + 1, ' '), request));
  // A 512-byte XPointer and a 64-byte device name are the limits.
  const std::string longest = "\"/body/DocFragment[" + std::string(Reading::MAX_XPOINTER_BYTES - 18, '1') + "\"";
  EXPECT_TRUE(parse(offerBody(good[0].c_str(), good[1].c_str(), longest.c_str(), good[3].c_str(),
                              ("\"" + std::string(64, 'a') + "\"").c_str()),
                    request));
}

TEST(ReadingProgressFormat, ListEntriesAreValidJsonWithinBudget) {
  char head[64];
  const size_t headSize = Reading::writeListHead("ABCD1234", head, sizeof(head));
  ASSERT_GT(headSize, 0u);
  std::string json(head, headSize);
  Reading::ListEntry entry;
  entry.path = "/Books/\"Quoted\" \\ 한글\t.epub";
  entry.document = DIGEST;
  entry.filenameDocument = DIGEST;
  entry.xpointer = "/body/DocFragment[3]/body/p[12]/text().5";
  entry.percentage = 0.43f;
  entry.updated = 1790000000;
  entry.seq = 17;
  char buffer[Reading::MAX_XPOINTER_BYTES + 512];
  size_t size = Reading::writeListEntry(entry, buffer, sizeof(buffer));
  ASSERT_GT(size, 0u);
  json.append(buffer, size);
  entry.xpointer = "";
  entry.updated = 0;
  size = Reading::writeListEntry(entry, buffer, sizeof(buffer));
  ASSERT_GT(size, 0u);
  json += ",";
  json.append(buffer, size);
  json += Reading::LIST_TAIL;

  JsonDocument doc;
  ASSERT_FALSE(deserializeJson(doc, json)) << json;
  EXPECT_EQ(doc["v"].as<int>(), 1);
  EXPECT_STREQ(doc["deviceID"], "ABCD1234");
  ASSERT_EQ(doc["books"].size(), 2u);
  EXPECT_STREQ(doc["books"][0]["path"], "/Books/\"Quoted\" \\ 한글\t.epub");
  EXPECT_STREQ(doc["books"][0]["progress"], "/body/DocFragment[3]/body/p[12]/text().5");
  EXPECT_NEAR(doc["books"][0]["percentage"].as<double>(), 0.43, 1e-5);
  EXPECT_EQ(doc["books"][0]["seq"].as<int>(), 17);
  EXPECT_TRUE(doc["books"][1]["progress"].isNull());
  EXPECT_EQ(doc["books"][1]["updated"].as<int>(), 0);

  // Ten books with the longest XPointers and long paths still fit only with the
  // budget the handler applies; each entry alone always fits the request buffer.
  const std::string longPath = "/" + std::string(250, 'p') + ".epub";
  const std::string longXPointer = "/body/DocFragment[1]" + std::string(Reading::MAX_XPOINTER_BYTES - 20, '1');
  entry.path = longPath.c_str();
  entry.xpointer = longXPointer.c_str();
  EXPECT_GT(Reading::writeListEntry(entry, buffer, sizeof(buffer)), 800u);
  EXPECT_EQ(Reading::writeListEntry(entry, buffer, 100), 0u);
  entry.document = "not-a-digest";
  EXPECT_EQ(Reading::writeListEntry(entry, buffer, sizeof(buffer)), 0u);
}

TEST(ReadingProgressFormat, OnlyClearlyFurtherOffersAreAsked) {
  EXPECT_TRUE(Reading::isFurther(0.5f, 0.4f));
  EXPECT_FALSE(Reading::isFurther(0.403f, 0.4f));
  EXPECT_FALSE(Reading::isFurther(0.3f, 0.4f));
}

TEST(ReadingProgressStore, RecordsAndOffersLiveInTheBookCache) {
  FakeSD::reset();
  auto scratch = std::make_unique<Reading::Scratch>();
  Reading::Record record;
  EXPECT_FALSE(Reading::loadRecord(CACHE, record, *scratch));
  ASSERT_TRUE(Reading::saveRecord(CACHE, sampleRecord(), *scratch));
  EXPECT_TRUE(FakeSD::files.count(std::string(CACHE) + "/pocket-reading.bin"));
  EXPECT_FALSE(FakeSD::files.count(std::string(CACHE) + "/pocket-reading.bin.tmp"));
  ASSERT_TRUE(Reading::loadRecord(CACHE, record, *scratch));
  EXPECT_EQ(record.page, 17);

  // A failed write keeps the previous record.
  FakeSD::failWriteOpen = true;
  Reading::Record changed = sampleRecord();
  changed.page = 18;
  EXPECT_FALSE(Reading::saveRecord(CACHE, changed, *scratch));
  FakeSD::failWriteOpen = false;
  ASSERT_TRUE(Reading::loadRecord(CACHE, record, *scratch));
  EXPECT_EQ(record.page, 17);

  EXPECT_FALSE(Reading::hasOffer(CACHE));
  ASSERT_TRUE(Reading::saveOffer(CACHE, sampleOffer(), *scratch));
  EXPECT_TRUE(Reading::hasOffer(CACHE));
  Reading::Offer offer;
  ASSERT_TRUE(Reading::loadOffer(CACHE, offer, *scratch));
  EXPECT_STREQ(offer.device, "Pocket Daily iPhone");
  Reading::removeOffer(CACHE);
  EXPECT_FALSE(Reading::hasOffer(CACHE));

  // Corrupt files never decode.
  FakeSD::files[std::string(CACHE) + "/pocket-reading-offer.bin"] = {1, 2, 3};
  EXPECT_FALSE(Reading::loadOffer(CACHE, offer, *scratch));
  EXPECT_FALSE(Reading::saveRecord("relative", sampleRecord(), *scratch));
}

TEST(ReadingProgressStore, SequenceAdvancesAndRecoversFromDamage) {
  FakeSD::reset();
  EXPECT_EQ(Reading::nextSequence(), 1u);
  EXPECT_EQ(Reading::nextSequence(), 2u);
  EXPECT_EQ(Reading::nextSequence(), 3u);
  FakeSD::files[Reading::SEQUENCE_PATH][0] ^= 1;
  EXPECT_EQ(Reading::nextSequence(), 1u);
  EXPECT_EQ(Reading::nextSequence(), 2u);
}
