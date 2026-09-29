// Pocket Reading Sync over BLE v1 records (docs/reading-sync-ble-v1.md).
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "pocket_daily/nearby_sync/ReadingSyncProtocol.h"

using namespace Pocket::NearbySync;

namespace {
ParseResult parse(const std::string& record, ParsedCommand& out) {
  return parseCommand(record.data(), record.size(), out);
}

// Hands out fixed pieces, like the reading list's head / entries / tail.
class PieceSource final : public ListChunker::Source {
 public:
  explicit PieceSource(std::vector<std::string> pieces) : pieces(std::move(pieces)) {}
  size_t nextPiece(char* out, const size_t capacity) override {
    if (index >= pieces.size()) return 0;
    const std::string& piece = pieces[index++];
    if (piece.size() > capacity) return 0;
    memcpy(out, piece.data(), piece.size());
    return piece.size();
  }

 private:
  std::vector<std::string> pieces;
  size_t index = 0;
};

uint32_t crcOf(const std::string& text) { return crcFinish(crcUpdate(CRC_START, text.data(), text.size())); }
}  // namespace

TEST(ReadingSyncProtocol, ParsesEveryVerb) {
  ParsedCommand command;
  ASSERT_EQ(parse("PING 0000000A", command), ParseResult::OK);
  EXPECT_EQ(command.verb, Verb::PING);
  EXPECT_STREQ(command.requestId, "0000000A");
  ASSERT_EQ(parse("START_AP DEADBEEF", command), ParseResult::OK);
  EXPECT_EQ(command.verb, Verb::START_AP);
  ASSERT_EQ(parse("CANCEL 12345678", command), ParseResult::OK);
  EXPECT_EQ(command.verb, Verb::CANCEL);
  ASSERT_EQ(parse("READ_LIST 0A0B0C0D", command), ParseResult::OK);
  EXPECT_EQ(command.verb, Verb::READ_LIST);

  ASSERT_EQ(parse("OFFER 0000000B 1024 cbf43926", command), ParseResult::OK);
  EXPECT_EQ(command.verb, Verb::OFFER);
  EXPECT_EQ(command.total, 1024u);
  EXPECT_EQ(command.crc, 0xCBF43926u);

  // The chunk is the rest of the record: spaces inside, at the start and end.
  const std::string record = "W 0000000B 12  \"device\":\"Pocket Daily iPhone\" ";
  ASSERT_EQ(parse(record, command), ParseResult::OK);
  EXPECT_EQ(command.verb, Verb::WRITE);
  EXPECT_EQ(command.seq, 12u);
  EXPECT_EQ(std::string(command.chunk, command.chunkLength), " \"device\":\"Pocket Daily iPhone\" ");
}

TEST(ReadingSyncProtocol, RejectsMalformedRecords) {
  ParsedCommand command;
  for (const char* bad : {"", "PING", "PING ", "PING 0000000a", "PING 000000A", "PING  0000000A", " PING 0000000A",
                          "PING 0000000AB", "READ_LIST 0000000A "}) {
    EXPECT_EQ(parse(bad, command), ParseResult::MALFORMED) << bad;
  }
  EXPECT_EQ(parse(std::string(RECORD_LIMIT + 1, 'A'), command), ParseResult::MALFORMED);
  EXPECT_EQ(parse("HELLO 0000000A", command), ParseResult::UNKNOWN_VERB);
  EXPECT_STREQ(command.requestId, "0000000A");

  for (const char* bad :
       {"PING 0000000A extra", "READ_LIST 0000000A x", "OFFER 0000000A 10", "OFFER 0000000A 10 1234567",
        "OFFER 0000000A 010 12345678", "OFFER 0000000A -1 12345678", "OFFER 0000000A 10 1234567G",
        "OFFER 0000000A 10 12345678 x", "W 0000000A 0", "W 0000000A 0 ", "W 0000000A x chunk", "W 0000000A 01 chunk"}) {
    EXPECT_EQ(parse(bad, command), ParseResult::BAD_FIELDS) << bad;
    EXPECT_STREQ(command.requestId, "0000000A");
  }
  EXPECT_STREQ(badFieldsCode(Verb::OFFER), "BAD_RECORD");
  EXPECT_STREQ(badFieldsCode(Verb::WRITE), "BAD_CHUNK");
  EXPECT_STREQ(badFieldsCode(Verb::READ_LIST), "BAD_COMMAND");
}

TEST(ReadingSyncProtocol, WindowsRefuseTheHotspot) {
  EXPECT_FALSE(allowedInWindow(Verb::START_AP));
  EXPECT_FALSE(allowedInWindow(Verb::NONE));
  for (const Verb verb : {Verb::PING, Verb::CANCEL, Verb::READ_LIST, Verb::OFFER, Verb::WRITE}) {
    EXPECT_TRUE(allowedInWindow(verb));
  }
  EXPECT_STREQ(NOT_IN_SYNC, "NOT_IN_SYNC");
}

TEST(ReadingSyncProtocol, FormatsEventRecordsWithinTheLimit) {
  char out[RECORD_LIMIT + 1];
  ASSERT_GT(formatOk("0000000A", out, sizeof(out)), 0u);
  EXPECT_STREQ(out, "OK 0000000A");
  ASSERT_GT(formatError("0000000A", "UNKNOWN_DOCUMENT", out, sizeof(out)), 0u);
  EXPECT_STREQ(out, "ERR 0000000A UNKNOWN_DOCUMENT");
  ASSERT_GT(formatError("bogus", "BAD_COMMAND", out, sizeof(out)), 0u);
  EXPECT_STREQ(out, "ERR 00000000 BAD_COMMAND");
  EXPECT_EQ(formatOk("bogus", out, sizeof(out)), 0u);

  ASSERT_GT(formatEnd("0000000A", 5321, 0x0000ABCDu, out, sizeof(out)), 0u);
  EXPECT_STREQ(out, "END 0000000A 5321 0000ABCD");

  const std::string chunk(MAX_DATA_CHUNK, 'x');
  const size_t length = formatData("0000000A", 4000000000u, chunk.data(), chunk.size(), out, sizeof(out));
  ASSERT_GT(length, 0u);
  EXPECT_LE(length, RECORD_LIMIT);
  EXPECT_EQ(std::string(out, 22), "D 0000000A 4000000000 ");
  EXPECT_EQ(formatData("0000000A", 0, chunk.data(), MAX_DATA_CHUNK + 1, out, sizeof(out)), 0u);
  EXPECT_EQ(formatData("0000000A", 0, chunk.data(), 0, out, sizeof(out)), 0u);
  EXPECT_EQ(formatData("0000000A", 0, chunk.data(), 10, out, 12), 0u);  // does not fit the buffer

  ASSERT_GT(formatStatus("X3", "89ABCDEF", "1.7.1", true, out, sizeof(out)), 0u);
  EXPECT_STREQ(out, "V=1;MODEL=X3;ID=89ABCDEF;FW=1.7.1;CAP=AP,HTTP,SD,COMMIT1,READ1;WIN=1");
  ASSERT_GT(formatStatus("X4", "89ABCDEF", "1.7.0-main-932d73fa", false, out, sizeof(out)), 0u);
  EXPECT_NE(strstr(out, "READ1;WIN=0"), nullptr);
  const std::string longFirmware(200, 'f');
  EXPECT_EQ(formatStatus("X4", "89ABCDEF", longFirmware.c_str(), false, out, sizeof(out)), 0u);
}

TEST(ReadingSyncProtocol, CrcIsZlibCrc32) {
  EXPECT_EQ(crcOf("123456789"), 0xCBF43926u);  // the standard CRC-32/IEEE check value
  EXPECT_EQ(crcOf(""), 0u);
  // Incremental updates equal one pass.
  uint32_t crc = crcUpdate(CRC_START, "1234", 4);
  crc = crcUpdate(crc, "56789", 5);
  EXPECT_EQ(crcFinish(crc), 0xCBF43926u);
}

TEST(ReadingSyncProtocol, ChunkerFramesPiecesIntoBoundedChunks) {
  std::vector<std::string> pieces = {"{\"v\":1,\"books\":[", std::string(500, 'a'), "," + std::string(179, 'b'), "]}"};
  std::string body;
  for (const auto& piece : pieces) body += piece;
  PieceSource source(pieces);
  char pieceBuffer[600];
  char chunk[MAX_DATA_CHUNK];
  ListChunker chunker;
  chunker.begin(pieceBuffer, sizeof(pieceBuffer));
  std::string framed;
  uint32_t chunks = 0;
  for (size_t length; (length = chunker.nextChunk(source, chunk)) != 0;) {
    EXPECT_LE(length, MAX_DATA_CHUNK);
    EXPECT_EQ(chunker.nextSeq(), ++chunks);
    framed.append(chunk, length);
  }
  EXPECT_TRUE(chunker.finished());
  EXPECT_EQ(framed, body);
  EXPECT_EQ(chunks, (body.size() + MAX_DATA_CHUNK - 1) / MAX_DATA_CHUNK);  // full chunks except the last
  EXPECT_EQ(chunker.totalBytes(), body.size());
  EXPECT_EQ(chunker.crc(), crcOf(body));
  EXPECT_EQ(chunker.nextChunk(source, chunk), 0u);

  PieceSource empty({});
  chunker.begin(pieceBuffer, sizeof(pieceBuffer));
  EXPECT_EQ(chunker.nextChunk(empty, chunk), 0u);
  EXPECT_EQ(chunker.totalBytes(), 0u);
  EXPECT_EQ(chunker.crc(), 0u);
}

TEST(ReadingSyncProtocol, AssemblerAcceptsOnlyTheAnnouncedBody) {
  const std::string body = "{\"device\":\"민수의 iPhone\",\"percentage\":0.5}";
  char buffer[MAX_OFFER_TOTAL + 1];
  OfferAssembler offer;
  ASSERT_TRUE(offer.begin("0000000C", body.size(), crcOf(body), buffer, sizeof(buffer)));
  EXPECT_TRUE(offer.matches("0000000C"));
  EXPECT_FALSE(offer.matches("0000000D"));
  // Chunks may split a multi-byte character.
  EXPECT_EQ(offer.write(0, body.data(), 13), OfferAssembler::Step::NEED_MORE);
  EXPECT_EQ(offer.write(1, body.data() + 13, 9), OfferAssembler::Step::NEED_MORE);
  EXPECT_EQ(offer.write(2, body.data() + 22, body.size() - 22), OfferAssembler::Step::COMPLETE);
  EXPECT_EQ(std::string(offer.body(), offer.length()), body);

  ASSERT_TRUE(offer.begin("0000000C", body.size(), crcOf(body), buffer, sizeof(buffer)));
  EXPECT_EQ(offer.write(1, body.data(), 4), OfferAssembler::Step::BAD_CHUNK);  // gap
  ASSERT_TRUE(offer.begin("0000000C", body.size(), crcOf(body), buffer, sizeof(buffer)));
  EXPECT_EQ(offer.write(0, body.data(), body.size()), OfferAssembler::Step::COMPLETE);
  ASSERT_TRUE(offer.begin("0000000C", 4, crcOf("abcd"), buffer, sizeof(buffer)));
  EXPECT_EQ(offer.write(0, "abcde", 5), OfferAssembler::Step::BAD_CHUNK);  // overrun
  ASSERT_TRUE(offer.begin("0000000C", 4, crcOf("abcd"), buffer, sizeof(buffer)));
  EXPECT_EQ(offer.write(0, "abce", 4), OfferAssembler::Step::BAD_CHUNK);  // CRC mismatch

  EXPECT_FALSE(offer.begin("0000000C", 0, 0, buffer, sizeof(buffer)));
  EXPECT_FALSE(offer.begin("0000000C", MAX_OFFER_TOTAL + 1, 0, buffer, sizeof(buffer)));
  EXPECT_FALSE(offer.begin("0000000C", 64, 0, buffer, 64));  // no room for the terminator
  EXPECT_FALSE(offer.begin("bogus", 4, 0, buffer, sizeof(buffer)));
  EXPECT_FALSE(offer.active());
}

TEST(ReadingSyncProtocol, RecordQueueIsBoundedAndPerConnection) {
  RecordQueue queue;
  char out[RECORD_LIMIT + 1];
  size_t length = 0;
  EXPECT_FALSE(queue.pop(out, sizeof(out), length, 1));
  for (int i = 0; i < static_cast<int>(RecordQueue::DEPTH); ++i) {
    const std::string record = "W 0000000A " + std::to_string(i) + " x";
    EXPECT_TRUE(queue.push(record.data(), record.size(), 1));
  }
  EXPECT_FALSE(queue.takeOverflow());
  EXPECT_FALSE(queue.push("PING 0000000A", 13, 1));
  EXPECT_TRUE(queue.takeOverflow());
  EXPECT_FALSE(queue.takeOverflow());  // reported once
  for (int i = 0; i < static_cast<int>(RecordQueue::DEPTH); ++i) {
    ASSERT_TRUE(queue.pop(out, sizeof(out), length, 1));
    EXPECT_EQ(std::string(out, length), "W 0000000A " + std::to_string(i) + " x");
  }
  EXPECT_FALSE(queue.pop(out, sizeof(out), length, 1));

  // Records from an earlier connection are dropped, newer ones kept in order.
  EXPECT_TRUE(queue.push("PING 00000001", 13, 1));
  EXPECT_TRUE(queue.push("PING 00000002", 13, 2));
  ASSERT_TRUE(queue.pop(out, sizeof(out), length, 2));
  EXPECT_STREQ(out, "PING 00000002");

  // Control characters never enter; UTF-8 bytes do.
  EXPECT_FALSE(queue.push("PING\n0000000A", 13, 2));
  EXPECT_FALSE(queue.push("PING\x7f", 5, 2));
  const std::string utf8 = "W 0000000A 0 \xEB\xAF\xBC";
  EXPECT_TRUE(queue.push(utf8.data(), utf8.size(), 2));
  EXPECT_FALSE(queue.push("", 0, 2));
  const std::string tooLong(RECORD_LIMIT + 1, 'A');
  EXPECT_FALSE(queue.push(tooLong.data(), tooLong.size(), 2));
  EXPECT_FALSE(queue.pop(out, RECORD_LIMIT, length, 2));  // caller buffer too small
  queue.clear();
  EXPECT_FALSE(queue.pop(out, sizeof(out), length, 2));
}
