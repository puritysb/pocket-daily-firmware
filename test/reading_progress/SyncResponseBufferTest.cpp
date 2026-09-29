#include <gtest/gtest.h>

#include <limits>
#include <string>

#include "KOReaderSync/SyncResponseBuffer.h"

TEST(SyncResponseBuffer, ChunksProduceOneTerminatedBody) {
  SyncResponseBuffer buffer;
  ASSERT_TRUE(buffer.ready());
  EXPECT_TRUE(buffer.append("{\"ok\":", 6));
  EXPECT_TRUE(buffer.append("true}", 5));
  EXPECT_STREQ(buffer.data(), "{\"ok\":true}");
  EXPECT_FALSE(buffer.failed());
}
TEST(SyncResponseBuffer, ExactLimitFitsAndOverflowLatches) {
  SyncResponseBuffer buffer;
  const std::string full(SyncResponseBuffer::CAPACITY, 'x');
  ASSERT_TRUE(buffer.append(full.data(), full.size()));
  EXPECT_EQ(std::strlen(buffer.data()), full.size());
  EXPECT_FALSE(buffer.append("x", 1));
  EXPECT_TRUE(buffer.failed());
  EXPECT_FALSE(buffer.append(nullptr, 0));
  EXPECT_EQ(std::strlen(buffer.data()), full.size());
}
TEST(SyncResponseBuffer, MalformedLengthCannotWrapOrCopy) {
  SyncResponseBuffer buffer;
  EXPECT_FALSE(buffer.append("x", std::numeric_limits<size_t>::max()));
  EXPECT_TRUE(buffer.failed());
  EXPECT_STREQ(buffer.data(), "");
}
TEST(SyncResponseBuffer, NullDataIsOnlyValidForEmptyChunk) {
  SyncResponseBuffer buffer;
  EXPECT_TRUE(buffer.append(nullptr, 0));
  EXPECT_FALSE(buffer.append(nullptr, 1));
  EXPECT_TRUE(buffer.failed());
}
