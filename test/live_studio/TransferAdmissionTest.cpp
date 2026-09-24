#include <gtest/gtest.h>

#include "pocket_daily/web/TransferAdmission.h"

using namespace PocketDaily::Web;

TEST(TransferAdmission, EveryLiveWriterBlocksMutation) {
  for (unsigned bits = 0; bits < 8; ++bits) {
    const TransferWriters writers{bool(bits & 1), bool(bits & 2), bool(bits & 4)};
    EXPECT_EQ(writers.any(), bits != 0) << bits;
  }
}

TEST(TransferAdmission, RejectionPersistsUntilNextRequestStart) {
  UploadAdmission admission;
  EXPECT_FALSE(admission.rejected());
  admission.begin(true);
  // WRITE, END and ABORT must all observe the same rejection even if the
  // competing writer finishes while this multipart body is still arriving.
  for (unsigned callback = 0; callback < 3; ++callback) EXPECT_TRUE(admission.rejected());
  admission.begin(false);
  EXPECT_FALSE(admission.rejected());
  admission.begin(true);
  EXPECT_TRUE(admission.rejected());
}
