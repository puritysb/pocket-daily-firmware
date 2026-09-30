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

namespace {
// UI_12_FONT_ID's family on the device (src/main.cpp), 1-bit and uncompressed.
EpdFont uiRegular(&ubuntu_12_regular);
EpdFont uiBold(&ubuntu_12_bold);
constexpr int UI_FONT = 12;
// The longest reader popup: EpubReaderActivity's build-error state clears the page first.
constexpr char INDEX_FAILED[] = "Failed to index - invalid book or SD error";

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
