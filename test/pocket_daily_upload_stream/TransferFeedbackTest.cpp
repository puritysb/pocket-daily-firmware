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
