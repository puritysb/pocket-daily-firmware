#include <GfxRenderer.h>
#include <gtest/gtest.h>
#include <uzlib.h>

#include <algorithm>
#include <climits>

#include "pocket_daily/ContentCard.h"
#include "pocket_daily/ContentPageRenderer.h"

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
