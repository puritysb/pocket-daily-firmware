#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "src/pocket_daily/upload_stream_protocol.h"

using namespace PocketDaily::UploadStream;

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
