#include <HalStorage.h>
#include <Memory.h>
#include <SdCardFont.h>
#include <gtest/gtest.h>

#include <cstring>

namespace {
using Bytes = std::vector<uint8_t>;
void put32(Bytes& bytes, size_t offset, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
}
// Two disjoint intervals: Latin A-J and Hangul 가. Same existing CPFONT v4.
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
class SdFont : public testing::Test {
 protected:
  void SetUp() override {
    FakeSD::reset();
    FakeFontMemory::fail = false;
    FakeFontMemory::maxRequest = 0;
    FakeSD::files["/test.cpfont"] = fontFile();
  }
};

TEST_F(SdFont, BoundedAndCachedModesUseTheSameGlyphMetricsAndPixels) {
  SdCardFont cached, bounded;
  ASSERT_TRUE(cached.load("/test.cpfont"));
  ASSERT_TRUE(bounded.load("/test.cpfont", SdCardFont::LoadMode::BoundedUI));
  for (uint32_t cp : {uint32_t('A'), uint32_t('J'), uint32_t(0xAC00)}) {
    const auto* a = cached.getEpdFont()->getGlyph(cp);
    const auto* b = bounded.getEpdFont()->getGlyph(cp);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(memcmp(a, b, sizeof(*a)), 0);
    ASSERT_NE(cached.getOverflowBitmap(a), nullptr);
    ASSERT_NE(bounded.getOverflowBitmap(b), nullptr);
    EXPECT_EQ(cached.getOverflowBitmap(a)[0], bounded.getOverflowBitmap(b)[0]);
  }
  EXPECT_EQ(bounded.getEpdFont()->getGlyph('Z'), nullptr);
  EXPECT_EQ(bounded.getEpdFont()->getGlyph(0xAC01), nullptr);
}

TEST_F(SdFont, BoundedPrewarmDoesNotCreatePageOrAdvanceTablesAndWrapsCache) {
  SdCardFont font;
  ASSERT_TRUE(font.load("/test.cpfont", SdCardFont::LoadMode::BoundedUI));
  const auto* stub = font.getEpdFont()->data;
  EXPECT_EQ(font.prewarm("ABCDEFGHIJ가", 1), 0);
  EXPECT_EQ(font.buildAdvanceTable("ABCDEFGHIJ가", 1), 0);
  EXPECT_FALSE(font.hasAdvanceTable());
  EXPECT_EQ(font.getEpdFont()->data, stub);
  EXPECT_EQ(font.prewarm("Z", 1), 1);
  ASSERT_NE(font.getEpdFont()->getGlyph('A'), nullptr);
  font.clearCache();
  EXPECT_EQ(font.getEpdFont()->data, stub);
  ASSERT_NE(font.getEpdFont()->getGlyph('J'), nullptr);
}

TEST_F(SdFont, ReadFailureDoesNotEvictAValidGlyph) {
  SdCardFont font;
  ASSERT_TRUE(font.load("/test.cpfont", SdCardFont::LoadMode::BoundedUI));
  const auto* saved = font.getEpdFont()->getGlyph('A');
  ASSERT_NE(saved, nullptr);
  const auto bitmap = font.getOverflowBitmap(saved)[0];
  FakeSD::readBudget = 0;
  EXPECT_EQ(font.getEpdFont()->getGlyph('B'), nullptr);
  EXPECT_TRUE(font.boundedReadFailed());
  EXPECT_EQ(font.getEpdFont()->getGlyph('A'), saved);
  EXPECT_EQ(font.getOverflowBitmap(saved)[0], bitmap);
}

TEST_F(SdFont, BoundedModeRejectsHugeGlyphAndOutOfFileBitmap) {
  for (int fault = 0; fault < 3; ++fault) {
    auto& bytes = FakeSD::files["/test.cpfont"];
    bytes = fontFile();
    if (fault == 0) {
      bytes[96] = 1;
      bytes[97] = 1;
    }  // 257-byte bitmap
    if (fault == 1) bytes[88] = 65;
    if (fault == 2) put32(bytes, 100, UINT32_MAX);
    SdCardFont font;
    ASSERT_TRUE(font.load("/test.cpfont", SdCardFont::LoadMode::BoundedUI));
    EXPECT_EQ(font.getEpdFont()->getGlyph('A'), nullptr);
  }
}

TEST_F(SdFont, RefusesInvalidIntervalsAndOffsets) {
  for (int fault = 0; fault < 5; ++fault) {
    auto& bytes = FakeSD::files["/test.cpfont"];
    bytes = fontFile();
    if (fault == 0) put32(bytes, 56, UINT32_MAX);
    if (fault == 1) put32(bytes, 56, 0);
    if (fault == 2) put32(bytes, 80, UINT32_MAX);
    if (fault == 3) put32(bytes, 84, 12);
    if (fault == 4) bytes.resize(60);
    SdCardFont font;
    EXPECT_FALSE(font.load("/test.cpfont", SdCardFont::LoadMode::BoundedUI));
    EXPECT_EQ(font.prewarm("A", 1), -1);
  }
}

TEST_F(SdFont, LoadFeedsProgressAndModeCanReturnToNormal) {
  static unsigned calls;
  calls = 0;
  SdCardFont font;
  ASSERT_TRUE(font.load("/test.cpfont", SdCardFont::LoadMode::BoundedUI, [] { ++calls; }));
  EXPECT_GE(calls, 2u);
  const auto before = calls;
  ASSERT_NE(font.getEpdFont()->getGlyph('A'), nullptr);
  EXPECT_GT(calls, before);
  ASSERT_TRUE(font.load("/test.cpfont"));
  EXPECT_EQ(font.loadMode(), SdCardFont::LoadMode::Cached);
  ASSERT_NE(font.getEpdFont()->getGlyph('A'), nullptr);
}

TEST_F(SdFont, DiskBackedKerningAndLigaturesMatchCachedShaping) {
  auto& bytes = FakeSD::files["/test.cpfont"];
  bytes.insert(bytes.begin() + 264, 15, 0);
  bytes[49] = 1;
  bytes[51] = 1;
  bytes[53] = 1;
  bytes[54] = 1;
  bytes[55] = 1;
  bytes[264] = 'A';
  bytes[266] = 1;
  bytes[267] = 'B';
  bytes[269] = 1;
  bytes[270] = static_cast<uint8_t>(-8);
  put32(bytes, 271, ('A' << 16) | 'B');
  put32(bytes, 275, 'C');
  SdCardFont cached, bounded;
  ASSERT_TRUE(cached.load("/test.cpfont"));
  ASSERT_TRUE(bounded.load("/test.cpfont", SdCardFont::LoadMode::BoundedUI));
  cached.prewarm("ABC", 1);  // implicit space is not in this deliberately tiny font
  const auto* normal = cached.getEpdFont();
  const auto* small = bounded.getEpdFont();
  EXPECT_EQ(normal->getKerning('A', 'B'), -8);
  EXPECT_EQ(small->getKerning('A', 'B'), normal->getKerning('A', 'B'));
  EXPECT_FALSE(bounded.boundedReadFailed());
  EXPECT_EQ(small->getKerning('B', 'A'), 0);
  EXPECT_EQ(small->getLigature('A', 'B'), normal->getLigature('A', 'B'));
  EXPECT_EQ(small->getLigature('A', 'B'), 'C');
  const char* remainder = "BJ";
  EXPECT_EQ(small->applyLigatures('A', remainder), 'C');
  EXPECT_STREQ(remainder, "J");
  // A corrupt class cannot index outside the matrix, nor may a corrupt
  // ligature emit an invalid scalar. No reads mutate the file.
  bytes[266] = 2;
  EXPECT_EQ(small->getKerning('A', 'B'), 0);
  EXPECT_TRUE(bounded.boundedReadFailed());
  put32(bytes, 275, UINT32_MAX);
  EXPECT_EQ(small->getLigature('A', 'B'), 0u);
  FakeSD::readBudget = 0;
  EXPECT_EQ(small->getKerning('A', 'B'), 0);
  EXPECT_EQ(small->getLigature('A', 'B'), 0u);
}

TEST_F(SdFont, BoundedGlyphAllocationFailurePreservesCacheAndCanRetry) {
  SdCardFont font;
  ASSERT_TRUE(font.load("/test.cpfont", SdCardFont::LoadMode::BoundedUI));
  const auto* saved = font.getEpdFont()->getGlyph('A');
  ASSERT_NE(saved, nullptr);
  FakeFontMemory::fail = true;
  EXPECT_EQ(font.getEpdFont()->getGlyph('B'), nullptr);
  EXPECT_TRUE(font.boundedReadFailed());
  EXPECT_EQ(font.getEpdFont()->getGlyph('A'), saved);
  FakeFontMemory::fail = false;
  EXPECT_NE(font.getEpdFont()->getGlyph('B'), nullptr);
  EXPECT_TRUE(font.boundedReadFailed());  // retry does not certify the old frame
  ASSERT_TRUE(font.load("/test.cpfont", SdCardFont::LoadMode::BoundedUI));
  EXPECT_FALSE(font.boundedReadFailed());
  EXPECT_LE(FakeFontMemory::maxRequest, SdCardFont::UI_GLYPH_BYTES);
}
}  // namespace
