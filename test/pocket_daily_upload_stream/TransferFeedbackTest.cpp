#include <gtest/gtest.h>

#include "pocket_daily/web/TransferFeedback.h"
using namespace PocketDaily::Web;
TEST(TransferCleanup, AcceptsOnlyCanonicalUUIDStagingNames) {
  EXPECT_TRUE(isTransferStagingName(".pocket-12345678-abcd-0123-4567-123456789abc.part"));
  for (const auto name :
       {"update.bin", "book.epub", ".pocket-backup.part", ".pocket-x.part",
        "../.pocket-12345678-abcd-0123-4567-123456789abc.part", ".pocket-12345678-Abcd-0123-4567-123456789abc.part",
        ".pocket-12345678_abcd-0123-4567-123456789abc.part"}) {
    EXPECT_FALSE(isTransferStagingName(name)) << name;
  }
}
TEST(TransferFeedback, PercentIsBoundedAndCannotOverflow) {
  TransferFeedback state;
  EXPECT_EQ(state.percent(), 0U);
  state.total = UINT32_MAX;
  state.received = UINT32_MAX;
  EXPECT_EQ(state.percent(), 100U);
  state.total = 10;
  EXPECT_EQ(state.percent(), 100U);
  state.received = 3;
  EXPECT_EQ(state.percent(), 30U);
}

TEST(TransferFeedback, DismissesOnlySuccessfulContentAfterFiveSeconds) {
  for (auto phase : {TransferPhase::Saved, TransferPhase::Removed}) {
    TransferFeedback state{phase, TransferKind::Content, 10, 10, 100};
    EXPECT_FALSE(state.shouldDismiss(5099));
    EXPECT_TRUE(state.shouldDismiss(5100));
    state.kind = TransferKind::Firmware;
    EXPECT_FALSE(state.shouldDismiss(90000));
  }
  for (auto phase : {TransferPhase::Idle, TransferPhase::Ready, TransferPhase::Receiving, TransferPhase::Verifying,
                     TransferPhase::Paused, TransferPhase::Failed}) {
    TransferFeedback state{phase, TransferKind::Content, 2, 10, 100};
    EXPECT_FALSE(state.shouldDismiss(90000));
  }
}
TEST(TransferFeedback, DismissalHandlesClockWrapAndANewTransfer) {
  TransferFeedback state{TransferPhase::Saved, TransferKind::Content, 10, 10, UINT32_MAX - 999};
  EXPECT_FALSE(state.shouldDismiss(3999));
  EXPECT_TRUE(state.shouldDismiss(4000));
  state.phase = TransferPhase::Receiving;
  EXPECT_FALSE(state.shouldDismiss(9000));
}
TEST(TransferFeedback, WireNamesMatchTheCompanionContract) {
  const TransferPhase phases[] = {TransferPhase::Idle,      TransferPhase::Ready, TransferPhase::Receiving,
                                  TransferPhase::Verifying, TransferPhase::Saved, TransferPhase::Paused,
                                  TransferPhase::Removed,   TransferPhase::Failed};
  const char* names[] = {"idle", "ready", "receiving", "verifying", "saved", "paused", "removed", "failed"};
  for (unsigned i = 0; i < 8; ++i) {
    TransferFeedback state;
    state.phase = phases[i];
    EXPECT_STREQ(state.phaseName(), names[i]);
  }
}
