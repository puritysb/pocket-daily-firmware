#include <gtest/gtest.h>

#include "src/network/FirmwareVersion.h"
#include "src/network/OtaFailure.h"

using OtaFailure::classify;
using OtaFailure::Kind;

namespace {
constexpr int kHttpConnect = 0x7002;  // ESP_ERR_HTTP_CONNECT
}

TEST(OtaFailure, TlsAllocationFailureIsMemoryWithEitherSign) {
  // Observed on X3: open ESP_ERR_HTTP_CONNECT with esp-tls code 0x7F00.
  EXPECT_EQ(classify(OtaFailure::kUpdaterHttpError, "open", kHttpConnect, 0x7F00, false), Kind::Memory);
  EXPECT_EQ(classify(OtaFailure::kUpdaterHttpError, "open", kHttpConnect, -0x7F00, false), Kind::Memory);
  EXPECT_EQ(classify(OtaFailure::kUpdaterHttpError, "open", kHttpConnect, -0x2880, false), Kind::Memory);
  EXPECT_EQ(classify(OtaFailure::kUpdaterHttpError, "open", OtaFailure::kEspErrNoMem, 0, false), Kind::Memory);
  EXPECT_EQ(classify(OtaFailure::kUpdaterHttpError, "buffer", 1024, 0, false), Kind::Memory);
  EXPECT_EQ(classify(OtaFailure::kUpdaterOomError, nullptr, 0, 0, true), Kind::Memory);
}

TEST(OtaFailure, OtherConnectFailuresAreNetwork) {
  EXPECT_EQ(classify(OtaFailure::kUpdaterHttpError, "open", kHttpConnect, 0, false), Kind::Network);
  EXPECT_EQ(classify(OtaFailure::kUpdaterHttpError, "open", kHttpConnect, -0x2700, false), Kind::Network);
  EXPECT_EQ(classify(OtaFailure::kUpdaterHttpError, "read", 512, 0, false), Kind::Network);
  EXPECT_EQ(classify(OtaFailure::kUpdaterHttpError, nullptr, 0, 0, false), Kind::Network);
}

TEST(OtaFailure, ServerAndReleaseProblems) {
  EXPECT_EQ(classify(OtaFailure::kUpdaterHttpError, "status", 403, 0, false), Kind::Server);
  EXPECT_EQ(classify(OtaFailure::kUpdaterHttpError, "redirects", 0, 0, false), Kind::Server);
  EXPECT_EQ(classify(OtaFailure::kUpdaterJsonParseError, nullptr, 0, 0, false), Kind::Release);
}

TEST(OtaFailure, InstallFailuresKeepTheirOwnReasonUnlessMemory) {
  EXPECT_EQ(classify(OtaFailure::kUpdaterHttpError, "status", 500, 0, true), Kind::Install);
  EXPECT_EQ(classify(OtaFailure::kUpdaterHttpError, "open", kHttpConnect, 0x7F00, true), Kind::Memory);
}

TEST(FirmwareVersion, ProductStableUpgradeNeverOffersHistoricalTags) {
  EXPECT_FALSE(FirmwareVersion::isNewerStable("v1.6.6", "1.0.0-beta.1"));
  EXPECT_FALSE(FirmwareVersion::isNewerStable("v1.7.0-beta.4", "1.0.0-beta.1"));
  EXPECT_TRUE(FirmwareVersion::isNewerStable("pocket-v1.0.0", "1.0.0-beta.1"));
  EXPECT_TRUE(FirmwareVersion::isNewerStable("pocket-v1.0.0", "1.0.0-dev-main-abcd"));
  EXPECT_FALSE(FirmwareVersion::isNewerStable("pocket-v1.0.0", "1.0.0"));
  EXPECT_TRUE(FirmwareVersion::isNewerStable("pocket-v1.0.1", "1.0.0"));
  EXPECT_FALSE(FirmwareVersion::isNewerStable("pocket-v1.0.0", "1.0.1"));
}

TEST(FirmwareVersion, RejectsMalformedAndPrereleaseLatestTags) {
  EXPECT_FALSE(FirmwareVersion::isNewerStable("pocket-v1.0.0-beta.1", "1.0.0"));
  EXPECT_FALSE(FirmwareVersion::isNewerStable("pocket-v1.0.0.1", "1.0.0"));
  EXPECT_FALSE(FirmwareVersion::isNewerStable("pocket-v10000.0.0", "1.0.0"));
  EXPECT_FALSE(FirmwareVersion::isNewerStable("pocket-v1.0.0", "bad"));
  EXPECT_FALSE(FirmwareVersion::isNewerStable(nullptr, "1.0.0"));
}
