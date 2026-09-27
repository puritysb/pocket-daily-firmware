#include <HalStorage.h>
#include <gtest/gtest.h>

#include <cstring>

#include "pocket_daily/BuildFailureLog.h"

namespace Log = PocketDaily::BuildFailureLog;

namespace {
Log::Entry sample() {
  Log::Entry entry;
  entry.book = 464604871;
  entry.spine = 5;
  entry.step = Log::STEP_HTML_STREAM;
  entry.detail = 5;  // ZipFile::StreamError::Window
  entry.freeHeap = 61234;
  entry.largestBlock = 28672;
  entry.uptimeSec = 812;
  std::strcpy(entry.version, "1.7.0-dev-feat-reader-support-7191e1a7-w21cca1f0");
  return entry;
}

class BuildFailureLog : public testing::Test {
 protected:
  void SetUp() override { FakeSD::reset(); }
};
}  // namespace

TEST_F(BuildFailureLog, RoundTripsEveryField) {
  uint8_t bytes[Log::RECORD_BYTES];
  ASSERT_EQ(Log::encode(sample(), bytes, sizeof(bytes)), Log::RECORD_BYTES);
  Log::Entry decoded;
  ASSERT_TRUE(Log::decode(bytes, sizeof(bytes), decoded));
  EXPECT_EQ(decoded.book, 464604871U);
  EXPECT_EQ(decoded.spine, 5);
  EXPECT_EQ(decoded.step, Log::STEP_HTML_STREAM);
  EXPECT_EQ(decoded.detail, 5);
  EXPECT_EQ(decoded.freeHeap, 61234U);
  EXPECT_EQ(decoded.largestBlock, 28672U);
  EXPECT_EQ(decoded.uptimeSec, 812U);
  EXPECT_STREQ(decoded.version, "1.7.0-dev-feat-reader-support-7191e1a7-w21cca1f0");
  EXPECT_STREQ(Log::stepName(decoded.step), "html-stream");
  EXPECT_STREQ(Log::detailName(decoded.step, decoded.detail), "window");
}

TEST_F(BuildFailureLog, RejectsDamagedOrForeignBytes) {
  uint8_t bytes[Log::RECORD_BYTES];
  ASSERT_EQ(Log::encode(sample(), bytes, sizeof(bytes)), Log::RECORD_BYTES);
  Log::Entry decoded;
  for (size_t i = 0; i < sizeof(bytes); ++i) {
    uint8_t damaged[Log::RECORD_BYTES];
    std::memcpy(damaged, bytes, sizeof(bytes));
    damaged[i] ^= 0x40;
    EXPECT_FALSE(Log::decode(damaged, sizeof(damaged), decoded)) << "byte " << i;
  }
  EXPECT_FALSE(Log::decode(bytes, sizeof(bytes) - 1, decoded));
  EXPECT_FALSE(Log::decode(nullptr, sizeof(bytes), decoded));
  Log::Entry bad = sample();
  bad.step = Log::STEP_COUNT;
  EXPECT_EQ(Log::encode(bad, bytes, sizeof(bytes)), 0U);
  EXPECT_EQ(Log::encode(sample(), bytes, sizeof(bytes) - 1), 0U);
}

TEST_F(BuildFailureLog, OverlongVersionStaysTerminated) {
  Log::Entry entry = sample();
  std::memset(entry.version, 'v', sizeof(entry.version));  // no terminator at all
  uint8_t bytes[Log::RECORD_BYTES];
  ASSERT_EQ(Log::encode(entry, bytes, sizeof(bytes)), Log::RECORD_BYTES);
  Log::Entry decoded;
  ASSERT_TRUE(Log::decode(bytes, sizeof(bytes), decoded));
  EXPECT_EQ(std::strlen(decoded.version), Log::VERSION_BYTES - 1);
}

TEST_F(BuildFailureLog, NamesOnlyMeaningfulDetails) {
  EXPECT_STREQ(Log::stepName(Log::STEP_LAYOUT), "layout");
  EXPECT_STREQ(Log::detailName(Log::STEP_LAYOUT, 1), "out-of-memory");
  EXPECT_EQ(Log::detailName(Log::STEP_LAYOUT, 0), nullptr);
  EXPECT_EQ(Log::detailName(Log::STEP_HTML_STREAM, 0), nullptr);
  EXPECT_EQ(Log::detailName(Log::STEP_HTML_STREAM, 200), nullptr);
  EXPECT_EQ(Log::detailName(Log::STEP_COMMIT, 5), nullptr);
  EXPECT_STREQ(Log::stepName(250), "unknown");
}

TEST_F(BuildFailureLog, RecordsAndCountsRepeatsOfTheSameChapter) {
  Log::Entry loaded;
  EXPECT_FALSE(Log::load(loaded));
  Log::Entry first = sample();
  ASSERT_TRUE(Log::record(first));
  EXPECT_EQ(first.count, 1);
  ASSERT_TRUE(Log::load(loaded));
  EXPECT_EQ(loaded.count, 1);
  Log::Entry again = sample();
  ASSERT_TRUE(Log::record(again));
  ASSERT_TRUE(Log::load(loaded));
  EXPECT_EQ(loaded.count, 2);
  Log::Entry other = sample();
  other.spine = 6;
  ASSERT_TRUE(Log::record(other));
  ASSERT_TRUE(Log::load(loaded));
  EXPECT_EQ(loaded.spine, 6);
  EXPECT_EQ(loaded.count, 1);
  EXPECT_EQ(FakeSD::files.count("/.crosspoint/last-build-error.tmp"), 0U);
  EXPECT_EQ(FakeSD::files.at(Log::PATH).size(), Log::RECORD_BYTES);
}

TEST_F(BuildFailureLog, FailedWriteKeepsThePreviousRecord) {
  Log::Entry first = sample();
  ASSERT_TRUE(Log::record(first));
  FakeSD::failWriteOpen = true;
  Log::Entry other = sample();
  other.spine = 9;
  EXPECT_FALSE(Log::record(other));
  FakeSD::failWriteOpen = false;
  Log::Entry loaded;
  ASSERT_TRUE(Log::load(loaded));
  EXPECT_EQ(loaded.spine, 5);
}
