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

// The reader records the CJK status-bar title with the page scan
// (ReaderPageRenderer, FontCacheManager::recordExtraText): the title's glyphs
// then load with the page's sorted batch instead of one SD open per glyph per
// draw through the overflow ring, and stay resident for the whole page render.
TEST_F(SdFontPageCache, StatusTextRecordedWithTheScanLoadsWithThePageGlyphs) {
  {
    auto page = cache.createPrewarmScope();
    renderer.drawText(FONT_ID, 0, 20, PAGE_TEXT);  // scan pass
    cache.recordExtraText(TITLE, FONT_ID);
    page.endScanAndPrewarm();
    const EpdGlyph* title = glyph(0xAC00);
    ASSERT_NE(title, nullptr);
    EXPECT_FALSE(font.isOverflowGlyph(title));
    EXPECT_EQ(title->advanceX, 48);
    for (const char* c = PAGE_TEXT; *c; ++c) {
      const EpdGlyph* g = glyph(static_cast<uint8_t>(*c));
      ASSERT_NE(g, nullptr);
      EXPECT_FALSE(font.isOverflowGlyph(g)) << *c;
    }
  }
  // Outside a scan the call is ignored.
  cache.recordExtraText(TITLE, FONT_ID);
  EXPECT_EQ(glyph(0xAC00) != nullptr && !font.isOverflowGlyph(glyph(0xAC00)), false);
}

namespace {
// Same font plus one kerning pair, A (left class 1) before B (right class 1): -3.
Bytes kernedFontFile() {
  Bytes bytes(64 + 24 + 11 * 16 + 3 + 3 + 1 + 11, 0);
  memcpy(bytes.data(), "CPFONT", 6);
  bytes[8] = 4;
  bytes[12] = 1;
  put32(bytes, 36, 2);
  put32(bytes, 40, 11);
  bytes[44] = 12;
  bytes[45] = 10;
  bytes[49] = 1;  // kernLeftEntryCount
  bytes[51] = 1;  // kernRightEntryCount
  bytes[53] = 1;  // kernLeftClassCount
  bytes[54] = 1;  // kernRightClassCount
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
  }
  const size_t kern = 88 + 11 * 16;
  bytes[kern + 0] = 'A';  // left entry: codepoint (LE 16) + class
  bytes[kern + 2] = 1;
  bytes[kern + 3] = 'B';  // right entry
  bytes[kern + 5] = 1;
  bytes[kern + 6] = static_cast<uint8_t>(-3);  // 1x1 matrix
  for (size_t i = 0; i < 11; ++i) bytes[kern + 7 + i] = static_cast<uint8_t>(0x80 + i);
  return bytes;
}

class SdFontExtraKerning : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(font.load("/kern.cpfont"));
    renderer.begin();
    renderer.setFontCacheManager(&cache);
    renderer.registerSdCardFont(FONT_ID, &font);
    renderer.insertFont(FONT_ID, EpdFontFamily(font.getEpdFont(0)));
  }
  int8_t kernAB() const { return renderer.getFontMap().at(FONT_ID).getKerning('A', 'B'); }

  const Bytes bytes = kernedFontFile();
  const Asset assets[1]{{"/kern.cpfont", bytes}};
  const AssetScope scope{assets};
  SdCardFont font;
  HalDisplay panel;
  GfxRenderer renderer{panel};
  FontCacheManager cache{renderer.getFontMap(), renderer.getSdCardFonts()};
};
}  // namespace

// Kerning is built per page from the page's codepoints; a glyph that is not on
// the page gets none. Extra (status-bar) text must not change that, or the
// title would shift by a kern pair depending on how it was loaded.
TEST_F(SdFontExtraKerning, ExtraTextGlyphsKeepTheirOnDemandKerning) {
  {
    auto page = cache.createPrewarmScope();
    renderer.drawText(FONT_ID, 0, 20, "AB");
    page.endScanAndPrewarm();
    EXPECT_EQ(kernAB(), -3);  // both on the page: kerned
  }
  {
    auto page = cache.createPrewarmScope();
    renderer.drawText(FONT_ID, 0, 20, "BCD");
    page.endScanAndPrewarm();
    EXPECT_EQ(kernAB(), 0);  // A not on the page (drawn on demand): unkerned
  }
  {
    auto page = cache.createPrewarmScope();
    renderer.drawText(FONT_ID, 0, 20, "BCD");
    cache.recordExtraText("A", FONT_ID);
    page.endScanAndPrewarm();
    const EpdGlyph* a = renderer.getFontMap().at(FONT_ID).getGlyph('A');
    ASSERT_NE(a, nullptr);
    EXPECT_FALSE(font.isOverflowGlyph(a));  // resident now...
    EXPECT_EQ(kernAB(), 0);                 // ...with the same (absent) kerning
  }
}

// Cross-page reuse: with a retain policy the page's glyph set survives the page render and
// the next prewarm copies the shared glyphs instead of reading them from SD. The copied
// records and bitmaps must equal what SD provides; the retained set is freed by that
// prewarm (or dropRetainedGlyphs), and without a policy nothing is retained.
TEST_F(SdFontPageCache, NextPagePrewarmReusesTheRetainedGlyphsExactly) {
  const auto renderPage = [&](const char* text) {
    auto page = cache.createPrewarmScope();
    renderer.drawText(FONT_ID, 0, 20, text);
    page.endScanAndPrewarm();
    std::vector<std::pair<EpdGlyph, std::vector<uint8_t>>> glyphs;
    for (const char* c = text; *c; ++c) {
      const EpdGlyph* g = glyph(static_cast<uint8_t>(*c));
      EXPECT_NE(g, nullptr);
      if (!g) continue;
      const uint8_t* bits = renderer.getGlyphBitmap(renderer.getFontMap().at(FONT_ID).getData(), g);
      EpdGlyph record = *g;
      record.dataOffset = 0;  // position inside the page buffer is not content
      glyphs.emplace_back(record, std::vector<uint8_t>(bits, bits + g->dataLength));
    }
    return glyphs;
  };
  const auto fresh = renderPage("CDEFG");

  cache.setRetainPolicy([](void*, uint32_t bytes) { return bytes > 0; }, nullptr);
  renderPage("ABCDE");
  EXPECT_GT(font.retainedGlyphBytes(), 0U);
  const auto reused = renderPage("CDEFG");
  EXPECT_EQ(font.getStats().reusedGlyphs, 3U);  // C, D, E
  ASSERT_EQ(reused.size(), fresh.size());
  for (size_t i = 0; i < fresh.size(); ++i) {
    EXPECT_EQ(memcmp(&reused[i].first, &fresh[i].first, sizeof(EpdGlyph)), 0) << i;
    EXPECT_EQ(reused[i].second, fresh[i].second) << i;
  }
  EXPECT_GT(font.retainedGlyphBytes(), 0U);  // this page is retained in turn
  cache.dropRetainedGlyphs();
  EXPECT_EQ(font.retainedGlyphBytes(), 0U);

  cache.setRetainPolicy(nullptr, nullptr);
  renderPage("ABCDE");
  EXPECT_EQ(font.retainedGlyphBytes(), 0U);
}
