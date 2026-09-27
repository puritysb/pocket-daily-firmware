#include <GfxRenderer.h>
#include <builtinFonts/ubuntu_12_bold.h>
#include <builtinFonts/ubuntu_12_regular.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "components/themes/PopupFrame.h"
#include "components/themes/lyra/LyraTheme.h"
#include "components/themes/roundedraff/RoundedRaffTheme.h"
#include "pocket_daily/live_studio/ThemeFieldIds.h"

namespace {
namespace Field = PocketDaily::LiveStudio::ThemeField;

// UI_12_FONT_ID's family on the device (src/main.cpp), 1-bit and uncompressed.
EpdFont uiRegular(&ubuntu_12_regular);
EpdFont uiBold(&ubuntu_12_bold);
constexpr int UI_FONT = 12;
// The longest reader popup: EpubReaderActivity's build-error state clears the page first.
constexpr char INDEX_FAILED[] = "Failed to index - invalid book or SD error";

// Classic with the Live Studio pack found active on the X3
// (/pocket-daily/ui-packs/studio-6c134a9368a44871.uipack): its popup records
// round the corners and keep bold text, but leave the text colour alone.
ThemeMetrics classicWithStudioPack() {
  ThemeMetrics metrics = BaseMetrics::values;
  Field::applyOverride(metrics, Field::kContentSidePadding, 1, 20);
  Field::applyOverride(metrics, Field::kHeaderHeight, 1, 44);
  Field::applyOverride(metrics, Field::kListRowHeight, 1, 56);
  Field::applyOverride(metrics, Field::kMenuRowHeight, 1, 60);
  Field::applyOverride(metrics, Field::kMenuSpacing, 1, 8);
  Field::applyOverride(metrics, Field::kPopupCornerRadius, 1, 8);
  Field::applyOverride(metrics, Field::kPopupTextBold, 2, 1);
  Field::applyOverride(metrics, Field::kTabBarHeight, 1, 40);
  return metrics;
}

struct Counts {
  size_t black = 0;
  size_t white = 0;
};

class PopupFrameHost : public testing::Test {
 protected:
  void SetUp() override {
    renderer.begin();
    renderer.insertFont(UI_FONT, EpdFontFamily(&uiRegular, &uiBold));
  }

  bool physicalBlack(const int x, const int y) const {
    const uint8_t* fb = panel.getFrameBuffer();
    return (fb[y * panel.getDisplayWidthBytes() + x / 8] & (0x80 >> (x % 8))) == 0;
  }

  // Bounding box of every black pixel on the physical panel.
  bool inkBounds(int& x0, int& y0, int& x1, int& y1) const {
    x0 = panel.getDisplayWidth();
    y0 = panel.getDisplayHeight();
    x1 = y1 = -1;
    for (int y = 0; y < panel.getDisplayHeight(); ++y) {
      for (int x = 0; x < panel.getDisplayWidth(); ++x) {
        if (!physicalBlack(x, y)) continue;
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
      }
    }
    return x1 >= 0;
  }

  Counts count(const int x0, const int y0, const int x1, const int y1) const {
    Counts counts;
    for (int y = y0; y <= y1; ++y) {
      for (int x = x0; x <= x1; ++x) (physicalBlack(x, y) ? counts.black : counts.white)++;
    }
    return counts;
  }

  // Physical framebuffer as PBM, for eyeballing against a photo of the panel.
  void writePbm(const char* name) const {
    const std::string path = std::string(POPUP_FRAME_OUTPUT) + "/" + name;
    FILE* out = std::fopen(path.c_str(), "wb");
    ASSERT_NE(out, nullptr) << path;
    std::fprintf(out, "P4\n%u %u\n", panel.getDisplayWidth(), panel.getDisplayHeight());
    const uint8_t* fb = panel.getFrameBuffer();
    for (uint32_t i = 0; i < panel.getBufferSize(); ++i) std::fputc(static_cast<uint8_t>(~fb[i]), out);
    std::fclose(out);
  }

  HalDisplay panel{792, 528};  // X3
  GfxRenderer renderer{panel};
};
}  // namespace

TEST(PopupFrameInk, NativeThemesKeepTheirPopups) {
  // Classic: black frame, white box, black text.
  const auto classic = PopupFrame::inkFor(BaseMetrics::values);
  EXPECT_TRUE(classic.frame);
  EXPECT_FALSE(classic.box);
  EXPECT_TRUE(classic.text);
  // Lyra and RoundedRaff: white ring, black rounded box, white text.
  for (const ThemeMetrics& metrics : {LyraMetrics::values, RoundedRaffMetrics::values}) {
    ASSERT_GT(metrics.popupCornerRadius, 0);
    const auto ink = PopupFrame::inkFor(metrics);
    EXPECT_FALSE(ink.frame);
    EXPECT_TRUE(ink.box);
    EXPECT_FALSE(ink.text);
  }
}

TEST(PopupFrameInk, TextAlwaysContrastsWithTheBoxWhateverThePackSets) {
  ThemeMetrics metrics = BaseMetrics::values;
  for (const int radius : {0, 8, 18}) {
    for (const bool textBlack : {false, true}) {
      Field::applyOverride(metrics, Field::kPopupCornerRadius, 1, radius);
      Field::applyOverride(metrics, Field::kPopupTextInverted, 2, textBlack ? 1 : 0);
      const auto ink = PopupFrame::inkFor(metrics);
      EXPECT_EQ(ink.text, textBlack);
      EXPECT_NE(ink.text, ink.box) << "radius " << radius;
      EXPECT_NE(ink.frame, ink.box) << "radius " << radius;
    }
  }
}

// The X3 report: reading in Landscape CW with Classic plus the studio pack, the
// build-error popup appeared as a solid black rounded bar along one long edge.
TEST_F(PopupFrameHost, RoundedClassicPopupShowsItsMessageInLandscapeOnX3) {
  const ThemeMetrics metrics = classicWithStudioPack();
  renderer.setOrientation(GfxRenderer::LandscapeClockwise);
  renderer.clearScreen();
  const Rect box = PopupFrame::draw(renderer, metrics, UI_FONT, INDEX_FAILED);
  EXPECT_EQ(panel.presentations, 0U);
  EXPECT_TRUE(panel.guardsIntact());
  writePbm("popup-x3-landscape-cw-classic-pack.pbm");

  // Logical geometry: a long, short box near the logical top.
  EXPECT_EQ(box.y, static_cast<int>(528 * metrics.popupTopOffsetRatio));
  EXPECT_GT(box.width, 400);
  EXPECT_LT(box.height, 80);

  int x0, y0, x1, y1;
  ASSERT_TRUE(inkBounds(x0, y0, x1, y1));
  const int frame = metrics.popupFrameThickness;
  EXPECT_EQ(x1 - x0 + 1, box.width + 2 * frame);
  EXPECT_EQ(y1 - y0 + 1, box.height + 2 * frame);

  // Inside the rounded corners, the box is white and carries black glyphs.
  const int inset = frame + metrics.popupCornerRadius;
  const Counts inside = count(x0 + inset, y0 + inset, x1 - inset, y1 - inset);
  EXPECT_GT(inside.black, 1000U) << "no message glyphs";
  EXPECT_GT(inside.white, inside.black) << "box filled in the text colour";
}

TEST_F(PopupFrameHost, NativeRoundedPopupStillDrawsWhiteTextOnBlack) {
  renderer.setOrientation(GfxRenderer::Portrait);
  renderer.clearScreen();
  const ThemeMetrics& metrics = LyraMetrics::values;
  PopupFrame::draw(renderer, metrics, UI_FONT, "Indexing");
  writePbm("popup-x3-portrait-lyra.pbm");
  int x0, y0, x1, y1;
  ASSERT_TRUE(inkBounds(x0, y0, x1, y1));
  const int inset = metrics.popupFrameThickness + metrics.popupCornerRadius;
  const Counts inside = count(x0 + inset, y0 + inset, x1 - inset, y1 - inset);
  EXPECT_GT(inside.white, 100U) << "no message glyphs";
  EXPECT_GT(inside.black, inside.white);
}
