#include <gtest/gtest.h>

#include "src/pocket_daily/product_identity.h"

TEST(PocketDailyIdentity, PublishesCanonicalProductIdentity) {
  EXPECT_STREQ(PocketDaily::PRODUCT_ID, "io.pocketdaily.reader");
  EXPECT_STREQ(PocketDaily::PRODUCT_NAME, "Pocket Daily");
}
