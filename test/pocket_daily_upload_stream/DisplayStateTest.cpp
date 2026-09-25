#include <gtest/gtest.h>

#include <string>

#include "src/pocket_daily/web/DisplayState.h"

using namespace PocketDaily::Web;

namespace {
DisplayInputs sample() {
  DisplayInputs in;
  in.deviceId = "5B09AF70";
  in.theme = themeName(1);
  in.orientation = 0;
  in.fontFamily = "PocketSansWorld";
  in.fontPointSize = 12;
  in.sidePadding = 20;
  in.topPadding = 5;
  in.spacing = 16;
  in.title = "Pocket";
  in.empty = "Pocket is ready. Connect briefly to refresh.";
  in.labels[0] = "Back";
  in.labels[1] = "";
  in.labels[2] = "Prev";
  in.labels[3] = "Next";
  return in;
}
}  // namespace

TEST(DisplayState, WritesTheResolvedContentPageInputs) {
  char out[1024];
  const auto n = writeDisplayJson(sample(), out, sizeof(out));
  ASSERT_GT(n, 0u);
  EXPECT_EQ(std::string(out, n),
            R"({"schema":1,"deviceID":"5B09AF70","theme":"lyra","orientation":0,)"
            R"("font":{"family":"PocketSansWorld","pointSize":12},)"
            R"("contentPage":{"sidePadding":20,"topPadding":5,"spacing":16,"title":"Pocket",)"
            R"("empty":"Pocket is ready. Connect briefly to refresh.","labels":["Back","","Prev","Next"]}})");
}

TEST(DisplayState, EscapesJsonAndKeepsUtf8Translations) {
  auto in = sample();
  in.title = "포켓 \"daily\"\\";
  in.labels[0] = "뒤로\n";
  char out[1024];
  const std::string json(out, writeDisplayJson(in, out, sizeof(out)));
  EXPECT_NE(json.find(R"("title":"포켓 \"daily\"\\")"), std::string::npos);
  EXPECT_NE(json.find(R"("labels":["뒤로\u000a")"), std::string::npos);
}

TEST(DisplayState, RejectsOverflowAndInvalidOrientationInsteadOfTruncating) {
  char small[64];
  EXPECT_EQ(writeDisplayJson(sample(), small, sizeof(small)), 0u);
  auto in = sample();
  in.orientation = 4;
  char out[1024];
  EXPECT_EQ(writeDisplayJson(in, out, sizeof(out)), 0u);
  EXPECT_EQ(writeDisplayJson(sample(), nullptr, 10), 0u);
}

TEST(DisplayState, ThemeNamesAreStableWireValues) {
  EXPECT_STREQ(themeName(0), "classic");
  EXPECT_STREQ(themeName(1), "lyra");
  EXPECT_STREQ(themeName(2), "lyra3covers");
  EXPECT_STREQ(themeName(3), "roundedraff");
  EXPECT_STREQ(themeName(9), "unknown");
}
