#include <gtest/gtest.h>

#include <array>
#include <cstring>

#include "pocket_daily/dev/ReaderWalk.h"

using namespace PocketDaily::DevCapture;

TEST(ReaderWalk, CountsCompletedDistinctPagesAcrossChaptersAndRequiresTurns) {
  ReaderWalk walk;
  ASSERT_TRUE(walk.begin(73, 24, 100));
  EXPECT_FALSE(walk.takeTurn(10000));  // No first page yet.
  for (uint32_t index = 0; index < 24; ++index) {
    const uint32_t now = 100 + index * 2000;
    if (index) {
      EXPECT_FALSE(walk.takeTurn(now - 1500));
      ASSERT_TRUE(walk.takeTurn(now));
      EXPECT_FALSE(walk.takeTurn(now));
      if (index > 1) walk.observe((index - 1) / 4, (index - 1) % 4 + 1, 0, now);
      EXPECT_EQ(walk.evidence.rendered, index);  // A stale page cannot satisfy a requested turn.
    }
    walk.observe(index / 4, index % 4 + 1, index, now);
    walk.observe(index / 4, index % 4 + 1, index, now);  // Repaint, not another page.
    ASSERT_EQ(walk.evidence.rendered, index + 1);
  }
  EXPECT_EQ(walk.evidence.state, WalkState::Completed);
  EXPECT_EQ(walk.evidence.turns, 23u);
  EXPECT_EQ(walk.evidence.spine, 5u);
  EXPECT_EQ(walk.evidence.page, 4u);
  EXPECT_FALSE(walk.takeTurn(100000));
}

TEST(ReaderWalk, RejectsBoundsAndStalledOrExpiredRunsIncludingClockWrap) {
  ReaderWalk walk;
  EXPECT_FALSE(walk.begin(0, 1, 0));
  EXPECT_FALSE(walk.begin(1, 0, 0));
  EXPECT_FALSE(walk.begin(1, 129, 0));
  ASSERT_TRUE(walk.begin(1, 128, UINT32_MAX - 1000));
  walk.tick(44000);
  EXPECT_EQ(walk.evidence.state, WalkState::TimedOut);
  ASSERT_TRUE(walk.begin(2, 128, 0));
  walk.observe(0, 1, 0, 0);
  for (uint32_t i = 1; i < 30; ++i) {
    ASSERT_TRUE(walk.takeTurn(i * 20000));
    walk.observe(i, 1, i, i * 20000);
  }
  walk.tick(WALK_TIMEOUT_MS);
  EXPECT_EQ(walk.evidence.state, WalkState::TimedOut);
}

TEST(ReaderWalk, FailureAfterFinalPaintOverridesCompletionAndEofIsNotSuccess) {
  ReaderWalk walk;
  ASSERT_TRUE(walk.begin(9, 1, 0));
  walk.observe(0, 1, 0, 1);
  ASSERT_EQ(walk.evidence.state, WalkState::Completed);
  walk.fail(1, 5, 19384, 8188, 2);
  EXPECT_EQ(walk.evidence.state, WalkState::BuildFailed);
  EXPECT_EQ(walk.evidence.step, 1u);
  EXPECT_EQ(walk.evidence.detail, 5u);
  EXPECT_EQ(walk.evidence.largestBlock, 8188u);
  ASSERT_TRUE(walk.begin(10, 2, 0));
  walk.observe(0, 1, 0, 1);
  ASSERT_TRUE(walk.takeTurn(1001));
  walk.turnResult(false);
  EXPECT_EQ(walk.evidence.state, WalkState::EndOfBook);
  EXPECT_EQ(walk.evidence.rendered, 1u);
  EXPECT_EQ(walk.evidence.turns, 0u);
}

TEST(ReaderWalk, DurableRecordRejectsTornCorruptAndImpossibleSuccess) {
  ReaderWalk walk;
  ASSERT_TRUE(walk.begin(0x10203040, 1, 0));
  walk.observe(3, 7, 29, 12);
  walk.evidence.captureReady = 1;
  walk.evidence.freeHeap = 23456;
  walk.evidence.largestBlock = 12345;
  uint8_t bytes[WALK_RECORD_BYTES];
  encodeWalk(walk.evidence, bytes);
  std::array<uint8_t, WALK_RECORD_BYTES + 1> unaligned{};
  memcpy(unaligned.data() + 1, bytes, sizeof(bytes));
  WalkEvidence decoded;
  ASSERT_TRUE(decodeWalk(unaligned.data() + 1, sizeof(bytes), decoded));
  EXPECT_EQ(decoded.run, 0x10203040u);
  EXPECT_EQ(decoded.spine, 3u);
  EXPECT_EQ(decoded.page, 7u);
  EXPECT_EQ(decoded.freeHeap, 23456u);
  EXPECT_EQ(decoded.largestBlock, 12345u);
  EXPECT_FALSE(decodeWalk(bytes, sizeof(bytes) - 1, decoded));
  bytes[49] ^= 1;  // Heap data stays semantically valid; only the checksum can reject it.
  EXPECT_FALSE(decodeWalk(bytes, sizeof(bytes), decoded));
  walk.evidence.captureReady = 0;
  encodeWalk(walk.evidence, bytes);
  EXPECT_FALSE(decodeWalk(bytes, sizeof(bytes), decoded));
  walk.evidence.captureReady = 1;
  walk.evidence.rendered = 0;
  encodeWalk(walk.evidence, bytes);
  EXPECT_FALSE(decodeWalk(bytes, sizeof(bytes), decoded));
}

TEST(ReaderWalk, BootAdmissionRequiresMatchingRunningReceiptAndPageTicket) {
  ReaderWalk walk;
  ASSERT_TRUE(walk.begin(73, 24, 0));
  EXPECT_TRUE(canBeginWalk(73, 24, walk.evidence));
  EXPECT_FALSE(canBeginWalk(74, 24, walk.evidence));
  EXPECT_FALSE(canBeginWalk(73, 23, walk.evidence));
  EXPECT_FALSE(canBeginWalk(73, 0, walk.evidence));
  walk.evidence.state = WalkState::Interrupted;
  EXPECT_FALSE(canBeginWalk(73, 24, walk.evidence));
  walk.evidence = {};
  EXPECT_FALSE(canBeginWalk(73, 24, walk.evidence));
}

TEST(ReaderWalk, SlowFinalRenderCannotTurnExpiredRunIntoSuccess) {
  ReaderWalk walk;
  ASSERT_TRUE(walk.begin(1, 1, 0));
  walk.observe(0, 1, 0, WALK_PAGE_TIMEOUT_MS - 1);
  EXPECT_EQ(walk.evidence.state, WalkState::Completed);
  ASSERT_TRUE(walk.begin(2, 1, 0));
  walk.observe(0, 1, 0, WALK_PAGE_TIMEOUT_MS);
  EXPECT_EQ(walk.evidence.state, WalkState::TimedOut);
  EXPECT_EQ(walk.evidence.rendered, 0u);
  ASSERT_TRUE(walk.begin(3, 2, 0));
  walk.observe(0, 1, 0, 1);
  EXPECT_FALSE(walk.takeTurn(WALK_PAGE_TIMEOUT_MS + 1));
  EXPECT_EQ(walk.evidence.state, WalkState::TimedOut);
}
