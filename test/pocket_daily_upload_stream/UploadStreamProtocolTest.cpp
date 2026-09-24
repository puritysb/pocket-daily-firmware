#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "src/pocket_daily/upload_stream_protocol.h"
#include "src/pocket_daily/web/TransferMetrics.h"

using namespace PocketDaily::UploadStream;

TEST(TransferMetrics, RetainsCompletedAttemptUntilNextConnection) {
  PocketDaily::Web::TransferMetrics metrics;
  using Outcome = PocketDaily::Web::TransferMetrics::Outcome;
  metrics.start(100, 12000, 6000);
  metrics.payload(110, 8192, 4096);
  metrics.service(115);
  metrics.receive(120, 4096);
  metrics.sdWrite.record(1500);
  metrics.heap(11000, 5000);
  metrics.heap(13000, 7000);
  metrics.end(130, Outcome::Complete, 8192);
  metrics.service(9999);
  metrics.end(9999, Outcome::Stopped, 0);
  EXPECT_EQ(metrics.outcome, Outcome::Complete);
  EXPECT_EQ(metrics.accepted, 8192u);
  EXPECT_EQ(metrics.socketBytes, 4096u);
  EXPECT_EQ(metrics.elapsedMs, 30u);
  EXPECT_EQ(metrics.maxServiceGapMs, 15u);
  EXPECT_EQ(metrics.maxReceiveGapMs, 10u);
  EXPECT_EQ(metrics.minHeap, 11000u);
  EXPECT_EQ(metrics.minBlock, 5000u);
  metrics.start(10000, 14000, 8000);
  EXPECT_EQ(metrics.attempt, 2u);
  EXPECT_EQ(metrics.socketBytes, 0u);
  EXPECT_EQ(metrics.sdWrite.calls, 0u);
  EXPECT_EQ(metrics.outcome, Outcome::Active);
}

TEST(TransferMetrics, WraparoundAndSaturationAreExplicit) {
  PocketDaily::Web::TransferMetrics metrics;
  using Outcome = PocketDaily::Web::TransferMetrics::Outcome;
  metrics.start(UINT32_MAX - 5, 100, 80);
  metrics.payload(UINT32_MAX - 5, 1000, 0);
  metrics.service(4);
  metrics.receive(4, 512);
  metrics.end(5, Outcome::Interrupted, 256);
  EXPECT_EQ(metrics.elapsedMs, 11u);
  EXPECT_EQ(metrics.maxServiceGapMs, 10u);
  EXPECT_EQ(metrics.maxReceiveGapMs, 10u);
  EXPECT_EQ(metrics.accepted, 256u);
  metrics.sdWrite.record(UINT32_MAX - 1);
  metrics.sdWrite.record(100);
  EXPECT_EQ(metrics.sdWrite.totalUs, UINT32_MAX);
  EXPECT_EQ(metrics.sdWrite.maxUs, UINT32_MAX - 1);
  EXPECT_EQ(metrics.sdWrite.calls, 2u);
}

TEST(TransferMetrics, WorstCaseJsonFitsBoundedChunks) {
  PocketDaily::Web::TransferMetrics metrics;
  metrics.attempt = metrics.expected = metrics.resumed = metrics.socketBytes = metrics.accepted = UINT32_MAX;
  metrics.elapsedMs = metrics.maxServiceGapMs = metrics.maxReceiveGapMs = metrics.minHeap = metrics.minBlock =
      UINT32_MAX;
  metrics.sdWrite = metrics.sdClose = metrics.replyWrite = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
  std::string json;
  for (unsigned section = 0; section < 3; ++section) {
    char chunk[224];
    const auto count = metrics.format(chunk, sizeof(chunk), section);
    ASSERT_GT(count, 0u);
    EXPECT_EQ(count, strlen(chunk));
    json.append(chunk, count);
    char tiny[2];
    EXPECT_EQ(metrics.format(tiny, sizeof(tiny), section), 0u);
  }
  EXPECT_EQ(json.front(), '{');
  EXPECT_EQ(json.back(), '}');
  EXPECT_NE(json.find("\"sdWriteUs\":[4294967295,4294967295,4294967295]"), std::string::npos);
  char chunk[224];
  EXPECT_EQ(metrics.format(chunk, sizeof(chunk), 3), 0u);
}

namespace {

bool parse(const std::string& header, Request& request) { return parseHeader(header.data(), header.size(), request); }

}  // namespace

TEST(UploadStreamProtocol, DetectsTerminatorOnlyOnBlankLine) {
  const std::string partial = "POCKET-PUT/1\nPath: /.pocket-a.part\nSize: 10\n";
  EXPECT_EQ(headerStatus(partial.data(), partial.size(), MAX_HEADER_BYTES), HeaderStatus::Incomplete);
  const std::string complete = partial + "\n";
  EXPECT_EQ(headerStatus(complete.data(), complete.size(), MAX_HEADER_BYTES), HeaderStatus::Complete);
  EXPECT_EQ(headerStatus(partial.data(), partial.size(), partial.size()), HeaderStatus::TooLarge);
}

TEST(UploadStreamProtocol, ParsesMinimalV1Header) {
  Request request;
  ASSERT_TRUE(parse("POCKET-PUT/1\nPath: /.pocket-3f2a.part\nSize: 2621440\n\n", request));
  EXPECT_STREQ(request.path, "/.pocket-3f2a.part");
  EXPECT_EQ(request.size, 2621440U);
  EXPECT_FALSE(request.resume);
  EXPECT_FALSE(request.flowControl);
}

TEST(UploadStreamProtocol, FlowControlRequiresNegotiatedWindowAndResumeHandshake) {
  Request request;
  const std::string base = "POCKET-PUT/1\nPath: /.pocket-a.part\nSize: 6000000\n";
  ASSERT_TRUE(parse(base + "Resume: 1\nWindow: 4096\n\n", request));
  EXPECT_TRUE(request.flowControl);
  EXPECT_FALSE(parse(base + "Window: 4096\n\n", request));
  EXPECT_FALSE(parse(base + "Resume: 1\nWindow: 0\n\n", request));
  EXPECT_FALSE(parse(base + "Resume: 1\nWindow: 8192\n\n", request));
  EXPECT_FALSE(parse(base + "Resume: 1\nWindow: 4096\nWindow: 4096\n\n", request));
  char reply[32];
  EXPECT_EQ(formatAckReply(reply, sizeof(reply), 131077), 11U);
  EXPECT_STREQ(reply, "ACK 131077\n");
  EXPECT_EQ(formatAckReply(reply, 4, 131077), 0U);
}

TEST(UploadStreamProtocol, ParsesResumeAndIgnoresUnknownKeys) {
  Request request;
  ASSERT_TRUE(parse("POCKET-PUT/1\nPath: /pocket-daily/learning/.pocket-x.part\nSize: 12\nClient: test\nResume: 1\n\n",
                    request));
  EXPECT_STREQ(request.path, "/pocket-daily/learning/.pocket-x.part");
  EXPECT_EQ(request.size, 12U);
  EXPECT_TRUE(request.resume);
}

TEST(UploadStreamProtocol, RejectsMalformedRecords) {
  Request request;
  EXPECT_FALSE(parse("POCKET-PUT/2\nPath: /.pocket-a.part\nSize: 1\n\n", request));
  EXPECT_FALSE(parse("POCKET-PUT/1\nSize: 1\n\n", request));
  EXPECT_FALSE(parse("POCKET-PUT/1\nPath: /.pocket-a.part\n\n", request));
  EXPECT_FALSE(parse("POCKET-PUT/1\nPath: /.pocket-a.part\nSize: 1\nSize: 2\n\n", request));
  EXPECT_FALSE(parse("POCKET-PUT/1\nPath: /.pocket-a.part\nSize: 4294967296\n\n", request));
  EXPECT_FALSE(parse("POCKET-PUT/1\nPath: /.pocket-a.part\nSize: -1\n\n", request));
  EXPECT_FALSE(parse("POCKET-PUT/1\nPath: /.pocket-a.part\nSize: 1\nResume: yes\n\n", request));
  EXPECT_FALSE(parse("POCKET-PUT/1\r\nPath: /.pocket-a.part\r\nSize: 1\r\n\r\n", request));
  EXPECT_FALSE(parse("POCKET-PUT/1\nPath: /.pocket-a.part\nSize: 1\n\ntrailing", request));
  EXPECT_FALSE(parse("POCKET-PUT/1\nPath: /.pocket-a.part\nSize: 1\nnot-a-record\n\n", request));
}

TEST(UploadStreamProtocol, AcceptsOnlyHiddenStagingPaths) {
  EXPECT_TRUE(isStagingPath("/.pocket-1.part"));
  EXPECT_TRUE(isStagingPath("/Books/.pocket-2c1.part"));
  EXPECT_FALSE(isStagingPath("/update.bin"));
  EXPECT_FALSE(isStagingPath("/.pocket-.part"));
  EXPECT_FALSE(isStagingPath(".pocket-1.part"));
  EXPECT_FALSE(isStagingPath("/../.pocket-1.part"));
  EXPECT_FALSE(isStagingPath("//.pocket-1.part"));
  EXPECT_FALSE(isStagingPath("/.pocket-1.part/"));
  EXPECT_FALSE(isStagingPath("/a\n/.pocket-1.part"));
  EXPECT_FALSE(isStagingPath(nullptr));
}

TEST(UploadStreamProtocol, Crc32MatchesCompanionWireFormat) {
  const char* sample = "123456789";
  uint32_t crc = updateCrc32(CRC32_INITIAL, reinterpret_cast<const uint8_t*>(sample), 9);
  EXPECT_EQ(finalizeCrc32(crc), 0xCBF43926U);

  // A resumed upload continues the running CRC across the retained prefix.
  uint32_t split = updateCrc32(CRC32_INITIAL, reinterpret_cast<const uint8_t*>(sample), 4);
  split = updateCrc32(split, reinterpret_cast<const uint8_t*>(sample + 4), 5);
  EXPECT_EQ(split, crc);
}

TEST(UploadStreamProtocol, FormatsBoundedReplies) {
  char out[32];
  EXPECT_EQ(formatOkReply(out, sizeof(out), 2621440U, 0xCBF43926U), 20U);
  EXPECT_STREQ(out, "OK 2621440 CBF43926\n");
  EXPECT_EQ(formatResumeReply(out, sizeof(out), 65536U), 13U);
  EXPECT_STREQ(out, "RESUME 65536\n");
  EXPECT_EQ(formatErrorReply(out, sizeof(out), "SD write failed"), 22U);
  EXPECT_STREQ(out, "ERROR SD write failed\n");
  EXPECT_EQ(formatErrorReply(out, 8, "SD write failed"), 0U);
}

#include "pocket_daily/direct_session.h"

TEST(DirectSession, ResponseMustDrainBeforeShutdown) {
  using PocketDaily::DirectSession::shouldEnd;
  EXPECT_FALSE(shouldEnd(false, 100, 10000));
  EXPECT_FALSE(shouldEnd(true, 100, 599));
  EXPECT_TRUE(shouldEnd(true, 100, 600));
  EXPECT_FALSE(shouldEnd(true, UINT32_MAX - 100, 398));
  EXPECT_TRUE(shouldEnd(true, UINT32_MAX - 100, 399));
}
