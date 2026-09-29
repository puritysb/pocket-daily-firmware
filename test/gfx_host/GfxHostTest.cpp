#include <GfxRenderer.h>
#include <gtest/gtest.h>
#include <uzlib.h>

#include <algorithm>
#include <climits>
#include <vector>

#include "components/themes/StatusBarTitle.h"
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

namespace {
// Logical pixel (x, y) is black, for every orientation (GfxRenderer::rotateCoordinates).
bool blackAt(const GfxRenderer& r, const int x, const int y) {
  const int w = r.getDisplayWidth(), h = r.getDisplayHeight();
  int px = x, py = y;
  switch (r.getOrientation()) {
    case GfxRenderer::Portrait:
      px = y, py = h - 1 - x;
      break;
    case GfxRenderer::LandscapeClockwise:
      px = w - 1 - x, py = h - 1 - y;
      break;
    case GfxRenderer::PortraitInverted:
      px = w - 1 - y, py = x;
      break;
    case GfxRenderer::LandscapeCounterClockwise:
      break;
  }
  return (r.getFrameBuffer()[py * r.getDisplayWidthBytes() + px / 8] & (0x80 >> (px & 7))) == 0;
}

int inkIn(const GfxRenderer& r, const int yFrom, const int yTo) {
  int ink = 0;
  for (int y = std::max(0, yFrom); y < std::min(r.getScreenHeight(), yTo); ++y)
    for (int x = 0; x < r.getScreenWidth(); ++x) ink += blackAt(r, x, y);
  return ink;
}

// Two 2-bit fonts with the device's metrics: the status bar's small UI font
// (notosans_8: ascender 18, descender 5) and a reader-size CJK SD font
// (PocketSansWorld 12: ascender 29, descender 8) whose glyphs fill that box.
struct StatusFonts {
  uint8_t smallBits[5 * 23 / 4 + 1];
  uint8_t bigBits[30 * 37 / 4 + 1];
  EpdGlyph smallGlyph{5, 23, 112, 0, 18, sizeof(smallBits), 0};
  EpdGlyph bigGlyph{30, 37, 512, 0, 29, sizeof(bigBits), 0};
  EpdUnicodeInterval smallInterval{'A', 'A', 0};
  EpdUnicodeInterval bigInterval{0x4E8C, 0x4E8C, 0};  // 二
  EpdFontData smallData{}, bigData{};
  EpdFont smallFont{&smallData}, bigFont{&bigData};
  StatusFonts() {
    std::fill(std::begin(smallBits), std::end(smallBits), 0xFF);
    std::fill(std::begin(bigBits), std::end(bigBits), 0xFF);
    smallData = {smallBits, &smallGlyph, &smallInterval, 1, 23, 18, -5, true};
    bigData = {bigBits, &bigGlyph, &bigInterval, 1, 37, 29, -8, true};
  }
};
}  // namespace

// Classic's status bar draws small text at textY = height - 19 - bottom margin - 4. A CJK
// title in the reader's SD font used the same y, so its baseline (y + 29) fell below the
// panel edge in landscape (X3 photo, 2026-09-28). It must stay inside the status lane,
// unclipped, in every orientation; a title font that fits keeps its old placement.
TEST(GfxHostStatusBarTitle, ReaderSizeCjkTitleStaysInsideTheStatusLane) {
  StatusFonts fonts;
  constexpr int SMALL = 1, BIG = 2;
  for (int o = 0; o < 4; ++o) {
    HalDisplay panel(792, 528);
    GfxRenderer renderer(panel);
    renderer.begin();
    renderer.insertFont(SMALL, EpdFontFamily(&fonts.smallFont));
    renderer.insertFont(BIG, EpdFontFamily(&fonts.bigFont));
    renderer.setOrientation(static_cast<GfxRenderer::Orientation>(o));
    int top, right, bottom, left;
    renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
    const int laneBottom = renderer.getScreenHeight() - bottom;
    const int textY = laneBottom - 19 - 4;

    // Before the fix: the title ran off the viewable area.
    renderer.clearScreen();
    renderer.drawText(BIG, 100, textY, "\xE4\xBA\x8C");
    EXPECT_GT(inkIn(renderer, laneBottom, renderer.getScreenHeight() + 64), 0) << o;

    const auto fit = StatusBarTitle::place(renderer, BIG, SMALL, textY);
    EXPECT_EQ(fit.scale, 2);
    renderer.clearScreen();
    renderer.drawText(BIG, 100, fit.y, "\xE4\xBA\x8C", true, fit.style);
    const int ink = inkIn(renderer, 0, renderer.getScreenHeight());
    ASSERT_GT(ink, 0) << o;
    EXPECT_EQ(inkIn(renderer, textY, laneBottom), ink) << "title outside the status lane, orientation " << o;
    // Nothing was clipped: the same title drawn mid-screen has the same ink.
    renderer.clearScreen();
    renderer.drawText(BIG, 100, 200, "\xE4\xBA\x8C", true, fit.style);
    EXPECT_EQ(inkIn(renderer, 0, renderer.getScreenHeight()), ink) << o;
    EXPECT_EQ(StatusBarTitle::width(renderer, BIG, "\xE4\xBA\x8C", fit),
              (renderer.getTextWidth(BIG, "\xE4\xBA\x8C") + 1) / 2);

    // A font whose line box fits the lane (the small font itself, or any flash title font) is unchanged.
    const auto same = StatusBarTitle::place(renderer, SMALL, SMALL, textY);
    EXPECT_EQ(same.y, textY);
    EXPECT_EQ(same.scale, 1);
    EXPECT_EQ(same.style, EpdFontFamily::REGULAR);
  }
}

TEST(GfxHost, LegacyRectangularIconsKeepExactPixelsInEveryOrientation) {
  static constexpr uint8_t icon[] = {0x7F, 0xBF, 0xFF};
  HalDisplay actual;
  HalDisplay expected;
  GfxRenderer renderer(actual);
  GfxRenderer reference(expected);
  renderer.begin();
  reference.begin();
  for (const auto orientation : {GfxRenderer::Portrait, GfxRenderer::PortraitInverted, GfxRenderer::LandscapeClockwise,
                                 GfxRenderer::LandscapeCounterClockwise}) {
    renderer.setOrientation(orientation);
    reference.setOrientation(orientation);
    renderer.clearScreen();
    reference.clearScreen();
    renderer.drawIcon(icon, 13, 17, 3, 2);
    reference.drawPixel(15, 17);
    reference.drawPixel(14, 18);
    EXPECT_EQ(memcmp(actual.getFrameBuffer(), expected.getFrameBuffer(), actual.getBufferSize()), 0);
    EXPECT_TRUE(actual.guardsIntact());
  }
}
