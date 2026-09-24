#include <gtest/gtest.h>

#include <string>

#include "agentdeck/OutboundQueue.h"

using AgentDeck::Net::OutboundQueue;

TEST(OutboundQueue, AllocatesOnlyForCommandsInActiveSession) {
  OutboundQueue queue;
  EXPECT_FALSE(queue.hasStorage());
  EXPECT_EQ(queue.push("old"), OutboundQueue::Result::Inactive);
  queue.beginSession();
  EXPECT_FALSE(queue.hasStorage());
  EXPECT_EQ(queue.push("new"), OutboundQueue::Result::Queued);
  EXPECT_TRUE(queue.hasStorage());
  queue.endSession();
  EXPECT_FALSE(queue.hasStorage());
}

TEST(OutboundQueue, DisconnectAndNewSessionDiscardPreviousCommands) {
  OutboundQueue queue;
  char line[OutboundQueue::LINE_BYTES];
  queue.beginSession();
  ASSERT_EQ(queue.push("old-approval"), OutboundQueue::Result::Queued);
  queue.endSession();
  EXPECT_FALSE(queue.pop(line, sizeof(line)));
  EXPECT_EQ(queue.push("offline-approval"), OutboundQueue::Result::Inactive);
  queue.beginSession();
  EXPECT_FALSE(queue.pop(line, sizeof(line)));
  ASSERT_EQ(queue.push("new-approval"), OutboundQueue::Result::Queued);
  ASSERT_TRUE(queue.pop(line, sizeof(line)));
  EXPECT_STREQ(line, "new-approval");
  ASSERT_EQ(queue.push("discard-on-replacement"), OutboundQueue::Result::Queued);
  queue.beginSession();
  EXPECT_FALSE(queue.pop(line, sizeof(line)));
  EXPECT_FALSE(queue.hasStorage());
}

TEST(OutboundQueue, RejectsTruncationAndPreservesFifoThroughWrap) {
  OutboundQueue queue;
  queue.beginSession();
  EXPECT_EQ(queue.push(nullptr), OutboundQueue::Result::Invalid);
  EXPECT_EQ(queue.push(""), OutboundQueue::Result::Invalid);
  EXPECT_EQ(queue.push(std::string(200, 'x').c_str()), OutboundQueue::Result::Invalid);
  EXPECT_FALSE(queue.hasStorage());
  for (size_t i = 0; i < OutboundQueue::CAPACITY; ++i)
    ASSERT_EQ(queue.push(std::to_string(i).c_str()), OutboundQueue::Result::Queued);
  EXPECT_EQ(queue.push("overflow"), OutboundQueue::Result::Full);
  char line[OutboundQueue::LINE_BYTES];
  EXPECT_FALSE(queue.pop(line, 1));
  for (size_t i = 0; i < 3; ++i) {
    ASSERT_TRUE(queue.pop(line, sizeof(line)));
    EXPECT_EQ(line, std::to_string(i));
    ASSERT_EQ(queue.push(std::to_string(i + 6).c_str()), OutboundQueue::Result::Queued);
  }
  for (size_t i = 3; i < 9; ++i) {
    ASSERT_TRUE(queue.pop(line, sizeof(line)));
    EXPECT_EQ(line, std::to_string(i));
  }
  EXPECT_FALSE(queue.pop(line, sizeof(line)));
  const std::string maximum(199, 'x');
  ASSERT_EQ(queue.push(maximum.c_str()), OutboundQueue::Result::Queued);
  ASSERT_TRUE(queue.pop(line, sizeof(line)));
  EXPECT_EQ(line, maximum);
}
