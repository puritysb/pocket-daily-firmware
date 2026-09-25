#include <gtest/gtest.h>

#include <string>

#include "src/pocket_daily/web/PreferencesUpdate.h"

using namespace PocketDaily::Web;

namespace {
constexpr PreferenceLimits kLimits{2, 1, 31, 4};

bool parse(const std::string& json, PreferencesUpdate& out, std::string* reason = nullptr) {
  const char* error = nullptr;
  const bool ok = parsePreferences(json.data(), json.size(), kLimits, out, error);
  if (reason && error) *reason = error;
  return ok;
}
}  // namespace

TEST(PreferencesUpdate, AcceptsTheCompanionsFullDocument) {
  PreferencesUpdate u;
  ASSERT_TRUE(parse(R"({"startupApp":1,"pocketDailySleepCover":1,"sleepTimeoutMinutes":10,"fontSize":1})", u));
  EXPECT_TRUE(u.hasStartupApp && u.hasSleepCover && u.hasSleepTimeout && u.hasFontSize);
  EXPECT_EQ(u.startupApp, 1);
  EXPECT_EQ(u.sleepCover, 1);
  EXPECT_EQ(u.sleepTimeoutMinutes, 10);
  EXPECT_EQ(u.fontSize, 1);
}

TEST(PreferencesUpdate, PartialDocumentsChangeOnlyPresentFieldsAndUnknownKeysAreIgnored) {
  PreferencesUpdate u;
  ASSERT_TRUE(parse(R"({"fontSize":3,"futureSetting":true})", u));
  EXPECT_FALSE(u.hasStartupApp);
  EXPECT_FALSE(u.hasSleepCover);
  EXPECT_FALSE(u.hasSleepTimeout);
  EXPECT_TRUE(u.hasFontSize);
  EXPECT_EQ(u.fontSize, 3);
}

TEST(PreferencesUpdate, SleepCoverKeepsBooleanAndIntegerCompatibility) {
  PreferencesUpdate u;
  ASSERT_TRUE(parse(R"({"pocketDailySleepCover":false})", u));
  EXPECT_EQ(u.sleepCover, 0);
  ASSERT_TRUE(parse(R"({"pocketDailySleepCover":2})", u));
  EXPECT_EQ(u.sleepCover, 1);
  EXPECT_FALSE(parse(R"({"pocketDailySleepCover":"on"})", u));
}

TEST(PreferencesUpdate, AnyInvalidFieldRejectsTheWholeDocument) {
  // The first field is valid; the document must still apply nothing.
  const std::string cases[] = {
      R"({"startupApp":1,"sleepTimeoutMinutes":0})",
      R"({"startupApp":1,"sleepTimeoutMinutes":32})",
      R"({"startupApp":2})",
      R"({"startupApp":"1"})",
      R"({"startupApp":1.5})",
      R"({"fontSize":4})",
      R"({"fontSize":-1})",
      R"([])",
      R"({)",
      "",
  };
  for (const auto& json : cases) {
    PreferencesUpdate u;
    u.hasStartupApp = true;  // sentinel: untouched on failure
    std::string reason;
    EXPECT_FALSE(parse(json, u, &reason)) << json;
    EXPECT_TRUE(u.hasStartupApp) << json;
    EXPECT_EQ(u.startupApp, 0) << json;
    EXPECT_FALSE(reason.empty()) << json;
  }
}
