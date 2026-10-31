#include <gtest/gtest.h>

#include <string>

#include "src/pocket_daily/FirmwareIdentity.h"
#include "src/pocket_daily/product_identity.h"

TEST(PocketDailyIdentity, PublishesCanonicalProductIdentity) {
  EXPECT_STREQ(PocketDaily::PRODUCT_ID, "io.pocketdaily.reader");
  EXPECT_STREQ(PocketDaily::PRODUCT_NAME, "Pocket Daily");
}

TEST(PocketDailyIdentity, DisplayVersionSeparatesReleaseBetaCandidateAndDeveloperImages) {
  char out[48];
  const auto show = [&](const char* full, size_t cap = sizeof(out)) {
    PocketDaily::FirmwareIdentity::displayVersion(full, out, cap);
    return std::string(out);
  };
  EXPECT_EQ(show("0.1.0"), "0.1.0");
  EXPECT_EQ(show("0.1.0-beta.3"), "0.1.0 beta 3");
  EXPECT_EQ(show("0.1.0-rc+4f2a91c"), "0.1.0 RC");
  EXPECT_EQ(show("0.1.0-dev-fix-reader-ui-polish-e7da898d-w9f0bb73a"), "0.1.0 dev e7da898d");
  EXPECT_EQ(show("0.1.0-dev-main-99f844c2"), "0.1.0 dev 99f844c2");
  EXPECT_EQ(show("0.1.0-dev-"), "0.1.0 dev");
  EXPECT_EQ(show("0.1.0-dev-main-99f844c2", 4), "0.1");
}
