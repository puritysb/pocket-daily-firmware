// Missing-glyph handling in the production renderer: invisible codepoints,
// emoji cluster collapsing, the fallback font chain and the missing mark.
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <SdCardFont.h>
#include <Utf8.h>
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <utility>
#include <vector>

using PocketUIHost::Asset;
using PocketUIHost::AssetScope;

namespace {
using Bytes = std::vector<uint8_t>;
void put16(Bytes& bytes, size_t offset, uint16_t value) {
  bytes[offset] = static_cast<uint8_t>(value);
  bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
}
void put32(Bytes& bytes, size_t offset, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
}

// CPFONT v4, one 1-bit style. Every glyph is 4x4 ink with a codepoint-derived
// pattern (so glyphs are distinguishable in the framebuffer) and `advance`.
Bytes buildFont(const std::vector<std::pair<uint32_t, uint32_t>>& intervals, uint16_t advance) {
  uint32_t glyphs = 0;
  for (const auto& [first, last] : intervals) glyphs += last - first + 1;
  const size_t dataOffset = 64;
  const size_t glyphOffset = dataOffset + intervals.size() * 12;
  const size_t bitmapOffset = glyphOffset + glyphs * 16;
  Bytes bytes(bitmapOffset + glyphs * 2, 0);
  memcpy(bytes.data(), "CPFONT", 6);
  bytes[8] = 4;
  bytes[12] = 1;
  put32(bytes, 36, static_cast<uint32_t>(intervals.size()));
  put32(bytes, 40, glyphs);
  bytes[44] = 20;  // advanceY
  bytes[45] = 16;  // ascender
  put32(bytes, 56, static_cast<uint32_t>(dataOffset));
  uint32_t index = 0;
  for (size_t i = 0; i < intervals.size(); ++i) {
    put32(bytes, dataOffset + i * 12, intervals[i].first);
    put32(bytes, dataOffset + i * 12 + 4, intervals[i].second);
    put32(bytes, dataOffset + i * 12 + 8, index);
    for (uint32_t cp = intervals[i].first; cp <= intervals[i].second; ++cp, ++index) {
      const size_t at = glyphOffset + index * 16;
      const bool blank = cp == ' ';
      bytes[at] = blank ? 0 : 4;
      bytes[at + 1] = blank ? 0 : 4;
      put16(bytes, at + 2, advance);
      put16(bytes, at + 6, blank ? 0 : 8);  // top
      put16(bytes, at + 8, blank ? 0 : 2);  // dataLength
      put32(bytes, at + 12, index * 2);
      bytes[bitmapOffset + index * 2] = static_cast<uint8_t>(0x90 | (cp & 0x0F));
      bytes[bitmapOffset + index * 2 + 1] = static_cast<uint8_t>(0x09 | ((cp >> 4) & 0x0F) << 4);
    }
  }
  return bytes;
}

constexpr uint32_t HANGUL_FIRST = 0xAC00;
constexpr uint32_t HANGUL_COUNT = 1800;
constexpr int PRIMARY = 7;
constexpr int BOUNDED = 8;

SdCardFont* gFallback = nullptr;
unsigned gProviderLoads = 0;
SdCardFont* provideFallback(void*, bool load) {
  if (load) ++gProviderLoads;
  return gFallback;
}

std::string utf8(std::initializer_list<uint32_t> cps) {
  std::string out;
  for (uint32_t cp : cps) utf8AppendCodepoint(cp, out);
  return out;
}

class GlyphFallback : public testing::Test {
 protected:
  void SetUp() override {
    gFallback = nullptr;
    gProviderLoads = 0;
    ASSERT_TRUE(primary.load("/primary.cpfont"));
    ASSERT_TRUE(fallback.load("/symbols.cpfont"));
    ASSERT_TRUE(bounded.load("/primary.cpfont", SdCardFont::LoadMode::BoundedUI));
    renderer.begin();
    renderer.setFontCacheManager(&cache);
    renderer.setFallbackFontProvider(&provideFallback, nullptr);
    renderer.registerSdCardFont(PRIMARY, &primary);
    renderer.insertFont(PRIMARY, EpdFontFamily(primary.getEpdFont(0)));
    renderer.registerSdCardFont(BOUNDED, &bounded);
    renderer.insertFont(BOUNDED, EpdFontFamily(bounded.getEpdFont(0)));
  }
  void TearDown() override { gFallback = nullptr; }

  Bytes draw(int fontId, const std::string& text) const {
    renderer.clearScreen();
    renderer.drawText(fontId, 4, 4, text.c_str());
    const uint8_t* fb = renderer.getFrameBuffer();
    return Bytes(fb, fb + renderer.getBufferSize());
  }
  bool blank(const Bytes& frame) const {
    for (uint8_t b : frame)
      if (b != 0xFF) return false;
    return true;
  }

  const Bytes primaryBytes = buildFont({{' ', ' '}, {'A', 'J'}, {HANGUL_FIRST, HANGUL_FIRST + HANGUL_COUNT - 1}}, 96);
  const Bytes fallbackBytes = buildFont({{0x2705, 0x2705}, {0x1F469, 0x1F469}}, 160);
  const Asset assets[2]{{"/primary.cpfont", primaryBytes}, {"/symbols.cpfont", fallbackBytes}};
  const AssetScope scope{assets};
  SdCardFont primary;
  SdCardFont fallback;
  SdCardFont bounded;
  HalDisplay panel;
  GfxRenderer renderer{panel};
  FontCacheManager cache{renderer.getFontMap(), renderer.getSdCardFonts()};
};

const std::string WOMAN = utf8({0x1F469});
const std::string WOMAN_TECHNOLOGIST = utf8({0x1F469, 0x1F3FD, 0x200D, 0x1F4BB});  // 👩🏽‍💻
const std::string LAPTOP = utf8({0x1F4BB});                                        // in no font

TEST_F(GlyphFallback, InvisibleCodepointsDrawNothingAndTakeNoWidth) {
  const std::string plain = "AB";
  const std::string laden = "A" + utf8({0x200B, 0x200D, 0x2060, 0xFE0F, 0x00AD, 0xE0041, 0x1F3FB}) + "B";
  EXPECT_EQ(draw(PRIMARY, laden), draw(PRIMARY, plain));
  EXPECT_EQ(renderer.getTextAdvanceX(PRIMARY, laden.c_str(), EpdFontFamily::REGULAR),
            renderer.getTextAdvanceX(PRIMARY, plain.c_str(), EpdFontFamily::REGULAR));
  EXPECT_EQ(renderer.getTextWidth(PRIMARY, laden.c_str()), renderer.getTextWidth(PRIMARY, plain.c_str()));
  // Same through the layout fast path (advance table resident).
  ASSERT_EQ(renderer.ensureSdCardFontReady(PRIMARY, laden.c_str(), 0x01), 0);
  EXPECT_EQ(renderer.getTextAdvanceX(PRIMARY, laden.c_str(), EpdFontFamily::REGULAR),
            renderer.getTextAdvanceX(PRIMARY, plain.c_str(), EpdFontFamily::REGULAR));
  EXPECT_EQ(gProviderLoads, 0u);  // covered text never asks for the fallback font
}

TEST_F(GlyphFallback, ZwjSequenceDrawsItsFirstComponentFromTheFallbackFont) {
  gFallback = &fallback;
  const Bytes sequence = draw(PRIMARY, "A" + WOMAN_TECHNOLOGIST + "B");
  EXPECT_EQ(sequence, draw(PRIMARY, "A" + WOMAN + "B"));
  EXPECT_NE(sequence, draw(PRIMARY, "AB"));
  const int expected = (96 + 160 + 96) / 16;
  EXPECT_EQ(renderer.getTextAdvanceX(PRIMARY, ("A" + WOMAN_TECHNOLOGIST + "B").c_str(), EpdFontFamily::REGULAR),
            expected);
  // Layout fast path: the fallback's advances are prepared with the paragraph.
  const std::vector<std::string> words = {"A" + WOMAN_TECHNOLOGIST + "B", utf8({0x2705})};
  // 👩 and ✅ are missing from the primary but the fallback covers them (💻 is a joined component).
  EXPECT_EQ(renderer.ensureSdCardFontReady(PRIMARY, words, false, 0x01), 0);
  EXPECT_TRUE(fallback.lookupAdvance(0x1F469, 0, nullptr));
  EXPECT_TRUE(fallback.lookupAdvance(0x2705, 0, nullptr));
  EXPECT_FALSE(fallback.lookupAdvance(0x1F4BB, 0, nullptr));  // joined component: never measured
  EXPECT_EQ(renderer.getTextAdvanceX(PRIMARY, words[0].c_str(), EpdFontFamily::REGULAR), expected);
}

TEST_F(GlyphFallback, UncoveredClusterDrawsOneSmallMark) {
  const int markPx = fp4::toPixel(GfxRenderer::missingGlyphAdvanceFP(primary.getEpdFont(0)->data));
  EXPECT_EQ(markPx, 8);
  // No fallback installed: one mark for the whole 👩🏽‍💻 cluster.
  const std::string text = "A" + WOMAN_TECHNOLOGIST + "B";
  EXPECT_EQ(renderer.ensureSdCardFontReady(PRIMARY, text.c_str(), 0x01), 1);  // one uncovered cluster
  const Bytes withMark = draw(PRIMARY, text);
  EXPECT_EQ(withMark, draw(PRIMARY, "A" + LAPTOP + "B"));
  EXPECT_NE(withMark, draw(PRIMARY, "AB"));
  EXPECT_EQ(renderer.getTextAdvanceX(PRIMARY, text.c_str(), EpdFontFamily::REGULAR), 6 + markPx + 6);
  // With a fallback that lacks the codepoint the result is the same mark.
  gFallback = &fallback;
  EXPECT_EQ(draw(PRIMARY, "A" + LAPTOP + "B"), withMark);
  // An uncovered combining mark adds nothing to its (drawn) base.
  EXPECT_EQ(draw(PRIMARY, "A" + utf8({0x0301}) + "B"), draw(PRIMARY, "AB"));
  // The mark is a light outline, drawn only in the BW pass.
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  EXPECT_TRUE(blank(draw(PRIMARY, LAPTOP)));
  renderer.setRenderMode(GfxRenderer::BW);
}

TEST_F(GlyphFallback, FallbackMissesAreAnsweredWithoutSdAccess) {
  gFallback = &fallback;
  const std::string text = LAPTOP + LAPTOP + LAPTOP + utf8({0x1F9FF, 0x1F4BB});
  draw(PRIMARY, text);  // warm up
  HalStorage::readOpenCount = 0;
  for (int pass = 0; pass < 5; ++pass) draw(PRIMARY, text);
  renderer.getTextAdvanceX(PRIMARY, text.c_str(), EpdFontFamily::REGULAR);
  EXPECT_EQ(HalStorage::readOpenCount, 0u);
}

TEST_F(GlyphFallback, PagePrewarmMakesFallbackGlyphsResidentForEveryStripPass) {
  gFallback = &fallback;
  const std::string text = "A" + WOMAN + utf8({0x2705}) + "B";
  auto page = cache.createPrewarmScope();
  renderer.drawText(PRIMARY, 4, 4, text.c_str());  // scan pass
  page.endScanAndPrewarm();
  const EpdGlyph* woman = fallback.getEpdFont(0)->findGlyph(0x1F469);
  ASSERT_NE(woman, nullptr);
  EXPECT_FALSE(fallback.isOverflowGlyph(woman));
  HalStorage::readOpenCount = 0;
  for (int strip = 0; strip < 4; ++strip) draw(PRIMARY, text);
  EXPECT_EQ(HalStorage::readOpenCount, 0u);
}

TEST_F(GlyphFallback, PageWithoutMissingGlyphsNeverLoadsTheFallback) {
  gFallback = &fallback;
  auto page = cache.createPrewarmScope();
  renderer.drawText(PRIMARY, 4, 4, "ABC");
  page.endScanAndPrewarm();
  draw(PRIMARY, "ABC");
  EXPECT_EQ(gProviderLoads, 0u);
}

TEST_F(GlyphFallback, BoundedUiFontsNeverUseTheFallback) {
  gFallback = &fallback;
  draw(BOUNDED, "A" + WOMAN + "B");
  renderer.getTextAdvanceX(BOUNDED, ("A" + WOMAN + "B").c_str(), EpdFontFamily::REGULAR);
  EXPECT_EQ(gProviderLoads, 0u);
  EXPECT_EQ(draw(BOUNDED, "A" + WOMAN + "B"), draw(BOUNDED, "A" + LAPTOP + "B"));  // both a mark
}

// Renderer-level regression for the X3 long-section stall: measuring a
// paragraph with more distinct syllables than the old 768-entry cap must not
// open the font per glyph.
TEST_F(GlyphFallback, LargeAlphabetParagraphMeasuresWithoutPerGlyphOpens) {
  std::vector<std::string> words;
  std::string word;
  for (uint32_t i = 0; i < HANGUL_COUNT; ++i) {
    utf8AppendCodepoint(HANGUL_FIRST + (i * 7919u) % HANGUL_COUNT, word);
    if (word.size() >= 9) {
      words.push_back(std::move(word));
      word.clear();
    }
  }
  HalStorage::readOpenCount = 0;
  ASSERT_EQ(renderer.ensureSdCardFontReady(PRIMARY, words, false, 0x01), 0);
  EXPECT_LE(HalStorage::readOpenCount, 2u);  // the uniform-interval scan batch, then the space
  HalStorage::readOpenCount = 0;
  int total = 0;
  for (const auto& w : words) total += renderer.getTextAdvanceX(PRIMARY, w.c_str(), EpdFontFamily::REGULAR);
  EXPECT_EQ(HalStorage::readOpenCount, 0u);
  EXPECT_EQ(total, static_cast<int>(HANGUL_COUNT) * 6);
}
}  // namespace
