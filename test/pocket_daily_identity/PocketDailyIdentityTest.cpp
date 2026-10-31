#include <gtest/gtest.h>

#include "src/pocket_daily/FirmwareIdentity.h"
#include "src/pocket_daily/product_identity.h"

TEST(PocketDailyIdentity, PublishesCanonicalProductIdentity) {
  EXPECT_STREQ(PocketDaily::PRODUCT_ID, "io.pocketdaily.reader");
  EXPECT_STREQ(PocketDaily::PRODUCT_NAME, "Pocket Daily");
}

TEST(PocketDailyIdentity, CompactVersionKeepsReleaseAndShortensDeveloperNames) {
  char out[48];
  PocketDaily::FirmwareIdentity::compactVersion("0.1.0", out, sizeof(out));
  EXPECT_STREQ(out, "0.1.0");
  PocketDaily::FirmwareIdentity::compactVersion("0.1.0-dev-fix-honor-chosen-reader-font-db0bf443-w9f0bb73a", out,
                                                sizeof(out));
  EXPECT_STREQ(out, "0.1.0-dev db0bf443");
  PocketDaily::FirmwareIdentity::compactVersion("0.1.0-dev-main-99f844c2", out, sizeof(out));
  EXPECT_STREQ(out, "0.1.0-dev 99f844c2");
  PocketDaily::FirmwareIdentity::compactVersion("1.6.5-rc+abc", out, sizeof(out));
  EXPECT_STREQ(out, "1.6.5-rc+abc");
  PocketDaily::FirmwareIdentity::compactVersion("0.1.0-dev-", out, sizeof(out));
  EXPECT_STREQ(out, "0.1.0-dev-");
  PocketDaily::FirmwareIdentity::compactVersion("0.1.0-dev-main-99f844c2", out, 4);
  EXPECT_STREQ(out, "0.1");
}
