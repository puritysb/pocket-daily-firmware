#include <gtest/gtest.h>

#include <string>

#include "src/pocket_daily/web/PreferencesUpdate.h"

using namespace PocketDaily::Web;

namespace {
constexpr PreferenceLimits kLimits{2, 1, 31, 4, 3};

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
      R"({"startupApp":1,"sideButtonLayout":3})",
      R"({"startupApp":1,"sideButtonLayout":-1})",
      R"({"startupApp":1,"sideButtonLayout":"1"})",
      R"({"startupApp":1,"frontButtonFollowOrientation":"on"})",
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

TEST(PreferencesUpdate, AcceptsTheHardwareButtonSettings) {
  PreferencesUpdate u;
  ASSERT_TRUE(parse(R"({"sideButtonLayout":0,"frontButtonFollowOrientation":true})", u));
  EXPECT_TRUE(u.hasSideButtonLayout && u.hasFrontButtonFollowOrientation);
  EXPECT_FALSE(u.hasStartupApp || u.hasSleepCover || u.hasSleepTimeout || u.hasFontSize);
  EXPECT_EQ(u.sideButtonLayout, 0);
  EXPECT_EQ(u.frontButtonFollowOrientation, 1);

  ASSERT_TRUE(parse(R"({"sideButtonLayout":1})", u));
  EXPECT_EQ(u.sideButtonLayout, 1);
  EXPECT_FALSE(u.hasFrontButtonFollowOrientation);

  // Upper bound: SIDE_BUTTONS_DISABLED (2) is the last valid layout.
  ASSERT_TRUE(parse(R"({"sideButtonLayout":2})", u));
  EXPECT_EQ(u.sideButtonLayout, 2);
}

TEST(PreferencesUpdate, SideButtonLayoutRejectsOutOfRangeAndNonIntegers) {
  PreferencesUpdate u;
  std::string reason;
  EXPECT_FALSE(parse(R"({"sideButtonLayout":3})", u, &reason));
  EXPECT_EQ(reason, "Invalid sideButtonLayout");
  EXPECT_FALSE(parse(R"({"sideButtonLayout":-1})", u));
  EXPECT_FALSE(parse(R"({"sideButtonLayout":"1"})", u));
  EXPECT_FALSE(parse(R"({"sideButtonLayout":true})", u));
  EXPECT_FALSE(parse(R"({"sideButtonLayout":1.5})", u));
  EXPECT_FALSE(u.hasSideButtonLayout);
}

TEST(PreferencesUpdate, FollowOrientationAcceptsBooleanAndInteger) {
  PreferencesUpdate u;
  ASSERT_TRUE(parse(R"({"frontButtonFollowOrientation":false})", u));
  EXPECT_TRUE(u.hasFrontButtonFollowOrientation);
  EXPECT_EQ(u.frontButtonFollowOrientation, 0);
  ASSERT_TRUE(parse(R"({"frontButtonFollowOrientation":1})", u));
  EXPECT_EQ(u.frontButtonFollowOrientation, 1);
  ASSERT_TRUE(parse(R"({"frontButtonFollowOrientation":7})", u));
  EXPECT_EQ(u.frontButtonFollowOrientation, 1);
  ASSERT_TRUE(parse(R"({"frontButtonFollowOrientation":0})", u));
  EXPECT_EQ(u.frontButtonFollowOrientation, 0);
  std::string reason;
  EXPECT_FALSE(parse(R"({"frontButtonFollowOrientation":"on"})", u, &reason));
  EXPECT_EQ(reason, "Invalid frontButtonFollowOrientation");
}

TEST(PreferencesUpdate, InvalidButtonFieldDoesNotReportEarlierValidFields) {
  // Every earlier field is valid; the invalid new field must leave `out`
  // exactly as it was, so nothing is reported present or applied.
  const std::string cases[] = {
      R"({"startupApp":1,"pocketDailySleepCover":true,"sleepTimeoutMinutes":10,"fontSize":1,"sideButtonLayout":3})",
      R"({"startupApp":1,"fontSize":2,"sideButtonLayout":1,"frontButtonFollowOrientation":"yes"})",
  };
  for (const auto& json : cases) {
    PreferencesUpdate u;
    EXPECT_FALSE(parse(json, u)) << json;
    EXPECT_FALSE(u.hasStartupApp || u.hasSleepCover || u.hasSleepTimeout || u.hasFontSize) << json;
    EXPECT_FALSE(u.hasSideButtonLayout || u.hasFrontButtonFollowOrientation) << json;
  }
}
