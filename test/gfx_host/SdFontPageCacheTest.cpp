#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <SdCardFont.h>
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

using PocketUIHost::Asset;
using PocketUIHost::AssetScope;

namespace {
using Bytes = std::vector<uint8_t>;
void put32(Bytes& bytes, size_t offset, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
}
// CPFONT v4, one style: Latin A-J and Hangul 가 (same layout as test/sd_font).
// Ten page glyphs exceed SdCardFont's eight-slot on-demand overflow ring.
Bytes fontFile() {
  Bytes bytes(64 + 24 + 11 * 16 + 11, 0);
  memcpy(bytes.data(), "CPFONT", 6);
  bytes[8] = 4;
  bytes[12] = 1;
  put32(bytes, 36, 2);
  put32(bytes, 40, 11);
  bytes[44] = 12;
  bytes[45] = 10;
  put32(bytes, 56, 64);
  put32(bytes, 64, 'A');
  put32(bytes, 68, 'J');
  put32(bytes, 76, 0xAC00);
  put32(bytes, 80, 0xAC00);
  put32(bytes, 84, 10);
  for (size_t i = 0; i < 11; ++i) {
    EpdGlyph glyph{};
    glyph.width = 2;
    glyph.height = 2;
    glyph.advanceX = 48;
    glyph.dataLength = 1;
    glyph.dataOffset = static_cast<uint32_t>(i);
    memcpy(bytes.data() + 88 + i * 16, &glyph, sizeof(glyph));
    bytes[264 + i] = static_cast<uint8_t>(0x80 + i);
  }
  return bytes;
}

constexpr int FONT_ID = 7;
constexpr char PAGE_TEXT[] = "ABCDEFGHIJ";
constexpr char TITLE[] = "\xEA\xB0\x80";  // 가, as a CJK status-bar chapter title

class SdFontPageCache : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(font.load("/page.cpfont"));
    renderer.begin();
    renderer.setFontCacheManager(&cache);
    renderer.registerSdCardFont(FONT_ID, &font);
    renderer.insertFont(FONT_ID, EpdFontFamily(font.getEpdFont(0)));
  }
  const EpdGlyph* glyph(uint32_t cp) const { return renderer.getFontMap().at(FONT_ID).getGlyph(cp); }

  const Bytes bytes = fontFile();
  const Asset assets[1]{{"/page.cpfont", bytes}};
  const AssetScope scope{assets};
  SdCardFont font;
  HalDisplay panel;
  GfxRenderer renderer{panel};
  FontCacheManager cache{renderer.getFontMap(), renderer.getSdCardFonts()};
};
}  // namespace

// Reader order (EpubReaderActivity/TxtReaderActivity::renderContents): scan + prewarm the page,
// draw it, draw the status bar (UiCjkFont reuses the reader font and prewarms the title), then
// redraw the page for every grayscale strip. The title prewarm must not evict the page glyphs,
// or each later strip reloads every glyph from SD through the overflow ring.
TEST_F(SdFontPageCache, StatusBarPrewarmDuringPageRenderKeepsPageGlyphsResident) {
  {
    auto page = cache.createPrewarmScope();
    renderer.drawText(FONT_ID, 0, 20, PAGE_TEXT);  // scan pass
    page.endScanAndPrewarm();
    ASSERT_TRUE(cache.isPageGlyphSetPinned());

    EXPECT_EQ(renderer.ensureSdCardFontReady(FONT_ID, TITLE, 0x01), 0);
    EXPECT_EQ(renderer.prewarmSdCardFont(FONT_ID, TITLE, 0x01), 0);

    for (const char* c = PAGE_TEXT; *c; ++c) {
      const EpdGlyph* g = glyph(static_cast<uint8_t>(*c));
      ASSERT_NE(g, nullptr);
      EXPECT_FALSE(font.isOverflowGlyph(g)) << "page glyph evicted: " << *c;
    }
    // The title still renders: its glyph loads on demand with the stored metrics.
    const EpdGlyph* title = glyph(0xAC00);
    ASSERT_NE(title, nullptr);
    EXPECT_TRUE(font.isOverflowGlyph(title));
    EXPECT_EQ(title->advanceX, 48);
  }
  EXPECT_FALSE(cache.isPageGlyphSetPinned());
}

TEST_F(SdFontPageCache, UiPrewarmOutsidePageRenderStillMakesGlyphsResident) {
  EXPECT_FALSE(cache.isPageGlyphSetPinned());
  renderer.prewarmSdCardFont(FONT_ID, TITLE, 0x01);  // the fixture has no U+FFFD, so 1 miss
  const EpdGlyph* title = glyph(0xAC00);
  ASSERT_NE(title, nullptr);
  EXPECT_FALSE(font.isOverflowGlyph(title));
}
