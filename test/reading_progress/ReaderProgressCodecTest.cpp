#include <gtest/gtest.h>

#include "activities/reader/ReaderProgressCodec.h"

TEST(ReaderProgressCodec, ReadsAllHistoricalFormats) {
  const uint8_t legacy[] = {2, 0, 4, 0, 12, 0, 63};
  ReaderProgressCodec::Position p;
  for (const size_t size : {4U, 6U, 7U}) {
    ASSERT_TRUE(ReaderProgressCodec::decode(legacy, size, p));
    EXPECT_EQ(p.spine, 2);
    EXPECT_EQ(p.page, 4);
    EXPECT_EQ(p.pageCount, size >= 6 ? 12 : 0);
    EXPECT_EQ(p.bookPercent, size == 7 ? 63 : -1);
    EXPECT_FALSE(p.visibleOffset);
  }
  const uint8_t upstream[] = {2, 0, 4, 0, 12, 0, 0x78, 0x56, 0x34, 0x12};
  ASSERT_TRUE(ReaderProgressCodec::decode(upstream, sizeof(upstream), p));
  EXPECT_EQ(p.visibleOffset, 0x12345678U);
  EXPECT_EQ(p.bookPercent, -1);
}
TEST(ReaderProgressCodec, ExtendedRecordPreservesAppPrefixAndZeroOffset) {
  ReaderProgressCodec::Position p{2, 4, 12, 63, 0};
  uint8_t data[ReaderProgressCodec::EXTENDED_SIZE]{};
  ASSERT_EQ(ReaderProgressCodec::encode(p, data), 12U);
  const uint8_t expected[] = {2, 0, 4, 0, 12, 0, 63, 0xD1, 0, 0, 0, 0};
  EXPECT_EQ(0, memcmp(data, expected, sizeof(expected)));
  ReaderProgressCodec::Position decoded;
  ASSERT_TRUE(ReaderProgressCodec::decode(data, sizeof(data), decoded));
  EXPECT_EQ(decoded.visibleOffset, 0U);
  EXPECT_EQ(decoded.bookPercent, 63);
}
TEST(ReaderProgressCodec, RejectsUnknownOrTruncatedRecordWithoutChangingOutput) {
  uint8_t data[13]{};
  ReaderProgressCodec::Position p{5, 6, 7, 8, 9};
  for (size_t size : {0U, 1U, 3U, 5U, 8U, 9U, 11U, 12U, 13U}) {
    EXPECT_FALSE(ReaderProgressCodec::decode(data, size, p));
    EXPECT_EQ(p.spine, 5);
    EXPECT_EQ(p.visibleOffset, 9U);
  }
}
TEST(ReaderProgressCodec, NormalizesLegacyLastPageSentinelAndUnknownPercent) {
  const uint8_t bytes[] = {0, 0, 0xFF, 0xFF, 1, 0, 0xFF};
  ReaderProgressCodec::Position p;
  ASSERT_TRUE(ReaderProgressCodec::decode(bytes, sizeof(bytes), p));
  EXPECT_EQ(p.page, 0);
  EXPECT_EQ(p.bookPercent, -1);
}
