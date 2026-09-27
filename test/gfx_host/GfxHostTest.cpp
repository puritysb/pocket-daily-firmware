#include <GfxRenderer.h>
#include <gtest/gtest.h>
#include <uzlib.h>

#include <algorithm>
#include <climits>
#include <vector>

#include "pocket_daily/ContentCard.h"
#include "pocket_daily/ContentPageRenderer.h"
#include "pocket_daily/home/HomeDrawing.h"

TEST(GfxHost, RealRasterizerKeepsPanelGuardsForEveryOrientation) {
  for (const auto [width, height] : {std::pair{800, 480}, {792, 528}}) {
    HalDisplay panel(width, height);
    GfxRenderer renderer(panel);
    renderer.begin();
    for (const auto orientation : {GfxRenderer::Portrait, GfxRenderer::LandscapeClockwise,
                                   GfxRenderer::PortraitInverted, GfxRenderer::LandscapeCounterClockwise}) {
      renderer.setOrientation(orientation);
      renderer.clearScreen();
      renderer.drawPixel(0, 0);
      renderer.drawPixel(renderer.getScreenWidth() - 1, renderer.getScreenHeight() - 1);
      renderer.drawRect(12, 12, 60, 40, 2);
      renderer.displayBuffer();
      EXPECT_TRUE(panel.guardsIntact());
      EXPECT_TRUE(std::any_of(panel.getFrameBuffer(), panel.getFrameBuffer() + panel.getBufferSize(),
                              [](auto b) { return b != 0xFF; }));
    }
    EXPECT_EQ(panel.presentations, 4U);
  }
}

TEST(GfxHost, HardwareOnlyOperationsFailExplicitly) {
  HalDisplay panel;
  EXPECT_FALSE(panel.supportsStripGrayscale());
  EXPECT_THROW(panel.preconditionGrayscale(), std::logic_error);
}

TEST(GfxHost, RejectsUnboundedOrNonByteAlignedPanelsBeforeAllocating) {
  EXPECT_THROW((HalDisplay(0, 480)), std::invalid_argument);
  EXPECT_THROW((HalDisplay(799, 480)), std::invalid_argument);
  EXPECT_THROW((HalDisplay(800, 0)), std::invalid_argument);
  EXPECT_THROW((HalDisplay(65528, 65535)), std::invalid_argument);
}

TEST(GfxHost, CheckedInflaterHostAdaptersPreserveIncrementalSeeds) {
  EXPECT_EQ(uzlib_adler32("123456789", 9, 1), 0x091E01DEU);
  EXPECT_EQ(~uzlib_crc32("123456789", 9, 0xFFFFFFFFU), 0xCBF43926U);
  EXPECT_EQ(uzlib_adler32("56789", 5, uzlib_adler32("1234", 4, 1)), 0x091E01DEU);
  EXPECT_EQ(~uzlib_crc32("56789", 5, uzlib_crc32("1234", 4, 0xFFFFFFFFU)), 0xCBF43926U);
}

class ContentPageHost : public testing::Test {
 protected:
  void SetUp() override {
    for (auto& glyph : glyphs) glyph = {4, 4, 64, 0, 4, 2, 0};
    data.bitmap = pixels;
    data.glyph = glyphs;
    data.intervals = &interval;
    data.intervalCount = 1;
    data.advanceY = 8;
    data.ascender = 4;
    renderer.begin();
    renderer.insertFont(1, EpdFontFamily(&font));
    std::strcpy(card.card.title, "Title");
    std::strcpy(card.card.question, "Question\nSecond line");
  }
  uint8_t pixels[2]{0xFF, 0xFF};
  EpdGlyph glyphs[95]{};
  EpdUnicodeInterval interval{32, 126, 0};
  EpdFontData data{};
  EpdFont font{&data};
  HalDisplay panel;
  GfxRenderer renderer{panel};
  PocketDaily::Content::ContentCard card{};
  PocketDaily::Content::ContentPageOptions options{1, 12, 8, 4, "Pocket", "No cards", {"Back", "", "Prev", "Next"}};
};

TEST_F(ContentPageHost, CompletePageUsesRealRasterizerWithoutDisplaying) {
  ASSERT_TRUE(PocketDaily::Content::renderContentPage(renderer, &card, options));
  EXPECT_EQ(panel.presentations, 0U);
  EXPECT_TRUE(panel.guardsIntact());
  EXPECT_TRUE(std::any_of(panel.getFrameBuffer(), panel.getFrameBuffer() + panel.getBufferSize(),
                          [](auto byte) { return byte != 0xFF; }));
  EXPECT_TRUE(PocketDaily::Content::renderContentPage(renderer, nullptr, options));
}

TEST_F(ContentPageHost, WeatherPlaceholderDrawsWithTheSelectedFontOnBothPanelSizes) {
  // Only font 1 is registered. Default font ids cannot draw these labels;
  // supplying the same resolver as Home must add their actual glyph pixels.
  const PocketDaily::HomeDraw::FontResolver fonts{nullptr,
                                                  [](void*, const char*, int, EpdFontFamily::Style) { return 1; }};
  for (const int width : {60, 200}) {
    renderer.clearScreen();
    PocketDaily::HomeDraw::drawWeatherPlaceholder(renderer, 20, 20, width, 160, "No weather", "Sync for forecast");
    const std::vector<uint8_t> withoutText(panel.getFrameBuffer(), panel.getFrameBuffer() + panel.getBufferSize());
    renderer.clearScreen();
    PocketDaily::HomeDraw::drawWeatherPlaceholder(renderer, 20, 20, width, 160, "No weather", "Sync for forecast",
                                                  fonts);
    EXPECT_NE(std::memcmp(withoutText.data(), panel.getFrameBuffer(), withoutText.size()), 0);
    EXPECT_TRUE(panel.guardsIntact());
    EXPECT_EQ(panel.presentations, 0U);
  }
}

TEST_F(ContentPageHost, InvalidOptionsDoNotTouchPriorFrame) {
  panel.clearScreen(0x55);
  options.sidePadding = INT_MAX;
  EXPECT_FALSE(PocketDaily::Content::renderContentPage(renderer, &card, options));
  options.sidePadding = 12;
  options.labels[3] = nullptr;
  EXPECT_FALSE(PocketDaily::Content::renderContentPage(renderer, &card, options));
  EXPECT_TRUE(std::all_of(panel.getFrameBuffer(), panel.getFrameBuffer() + panel.getBufferSize(),
                          [](auto byte) { return byte == 0x55; }));
}

TEST_F(ContentPageHost, ImageCallbackIsBoundedAndFailuresCannotBecomeSuccessfulPage) {
  std::strcpy(card.imagePath, "images/card.pbm");
  EXPECT_FALSE(PocketDaily::Content::renderContentPage(renderer, &card, options));
  unsigned calls = 0;
  PocketDaily::Content::ContentPageImagePainter images{
      &calls, [](void* context, const char* name, int x, int y, int width, int height) {
        ++*static_cast<unsigned*>(context);
        EXPECT_STREQ(name, "images/card.pbm");
        EXPECT_GE(x, 0);
        EXPECT_GE(y, 0);
        EXPECT_GT(width, 0);
        EXPECT_GT(height, 0);
        EXPECT_LE(x + width, 480);
        EXPECT_LT(y + height, 800);
        return false;
      }};
  EXPECT_FALSE(PocketDaily::Content::renderContentPage(renderer, &card, options, images));
  EXPECT_EQ(calls, 1U);
  EXPECT_EQ(panel.presentations, 0U);
  EXPECT_TRUE(panel.guardsIntact());
}

// Tiled grayscale culls a text line from strips its BW ink rows miss
// (ReaderPageRenderer). That is exact only if every pixel a grayscale pass
// draws lies on a row the BW pass drew: check it for 2-bit anti-aliased glyphs
// (black, both grays, a glyph taller than the ascender) in all four orientations.
TEST(GfxHostInkRows, GrayscalePassesStayWithinTheBwInkRows) {
  // 8x8 2-bit glyphs (16 B each): rows cycle through white, light, dark, black.
  uint8_t bitmap[2 * 16];
  for (size_t i = 0; i < sizeof(bitmap); ++i) bitmap[i] = static_cast<uint8_t>(0x1B + i * 37);
  EpdGlyph glyphs[2] = {{8, 8, 144, 0, 6, 16, 0}, {8, 8, 144, -1, 11, 16, 16}};  // 'A', 'B' (top 11 > ascender)
  EpdUnicodeInterval interval{'A', 'B', 0};
  EpdFontData data{};
  data.bitmap = bitmap;
  data.glyph = glyphs;
  data.intervals = &interval;
  data.intervalCount = 1;
  data.advanceY = 12;
  data.ascender = 8;
  data.is2Bit = true;
  EpdFont font{&data};
  const char* text = "ABBAB";
  for (int o = 0; o < 4; ++o) {
    HalDisplay panel(792, 528);
    GfxRenderer renderer(panel);
    renderer.begin();
    renderer.insertFont(1, EpdFontFamily(&font));
    renderer.setOrientation(static_cast<GfxRenderer::Orientation>(o));
    renderer.beginInkRows();
    renderer.drawText(1, 30, 100, text);
    int first = 0, last = -1;
    ASSERT_TRUE(renderer.endInkRows(&first, &last)) << o;
    int grayRows = 0;
    std::vector<uint8_t> strip(static_cast<size_t>(renderer.getDisplayWidthBytes()) * 4);
    for (const auto mode : {GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
      renderer.setRenderMode(mode);
      for (int y = 0; y < renderer.getDisplayHeight(); y += 4) {
        renderer.beginStripTarget(strip.data(), y, 4);
        renderer.clearScreen(0x00);
        renderer.drawText(1, 30, 100, text);
        renderer.endStripTarget();
        const bool touched = std::any_of(strip.begin(), strip.end(), [](uint8_t b) { return b != 0; });
        if (touched) ++grayRows;
        if (y + 3 < first || y > last) EXPECT_FALSE(touched) << "orientation " << o << " strip " << y;
      }
    }
    EXPECT_GT(grayRows, 0) << "fixture must draw gray pixels";
    renderer.setRenderMode(GfxRenderer::BW);
    // Tracking stopped at endInkRows: later drawing does not widen the range,
    // and a new tracking window that draws nothing reports no ink.
    renderer.drawText(1, 30, 300, "A");
    int again = 0, againLast = -1;
    renderer.endInkRows(&again, &againLast);
    EXPECT_EQ(again, first);
    EXPECT_EQ(againLast, last);
    renderer.beginInkRows();
    EXPECT_FALSE(renderer.endInkRows(&again, &againLast));
  }
}
