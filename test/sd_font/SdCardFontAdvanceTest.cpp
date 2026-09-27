// Layout advance cache: large-alphabet chapters must not fall back to one SD
// open per glyph, and the cache must stay bounded.
#include <HalStorage.h>
#include <Memory.h>
#include <SdCardFont.h>
#include <Utf8.h>
#include <gtest/gtest.h>

#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace {
using Bytes = std::vector<uint8_t>;

void put16(Bytes& bytes, size_t offset, uint16_t value) {
  bytes[offset] = static_cast<uint8_t>(value);
  bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
}
void put32(Bytes& bytes, size_t offset, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
}

constexpr uint32_t HANGUL_FIRST = 0xAC00;
constexpr uint32_t HANGUL_COUNT = 2400;  // one interval, above UNIFORM_SCAN_MIN_SPAN

// CPFONT v4, one style: ASCII space..'~' plus HANGUL_COUNT syllables, every
// glyph 2x2 with one bitmap byte. `advance` gives each glyph's 12.4 advance.
Bytes largeFont(const std::function<uint16_t(uint32_t)>& advance) {
  const std::vector<std::pair<uint32_t, uint32_t>> intervals = {{0x20, 0x7E},
                                                                {HANGUL_FIRST, HANGUL_FIRST + HANGUL_COUNT - 1}};
  uint32_t glyphs = 0;
  for (const auto& [first, last] : intervals) glyphs += last - first + 1;
  const size_t dataOffset = 64;
  const size_t glyphOffset = dataOffset + intervals.size() * 12;
  const size_t bitmapOffset = glyphOffset + glyphs * 16;
  Bytes bytes(bitmapOffset + glyphs, 0);
  memcpy(bytes.data(), "CPFONT", 6);
  bytes[8] = 4;
  bytes[12] = 1;
  put32(bytes, 36, static_cast<uint32_t>(intervals.size()));
  put32(bytes, 40, glyphs);
  bytes[44] = 36;  // advanceY
  bytes[45] = 29;  // ascender
  put32(bytes, 56, static_cast<uint32_t>(dataOffset));
  uint32_t index = 0;
  for (size_t i = 0; i < intervals.size(); ++i) {
    put32(bytes, dataOffset + i * 12, intervals[i].first);
    put32(bytes, dataOffset + i * 12 + 4, intervals[i].second);
    put32(bytes, dataOffset + i * 12 + 8, index);
    for (uint32_t cp = intervals[i].first; cp <= intervals[i].second; ++cp, ++index) {
      const size_t at = glyphOffset + index * 16;
      bytes[at] = 2;
      bytes[at + 1] = 2;
      put16(bytes, at + 2, advance(cp));
      put16(bytes, at + 8, 1);
      put32(bytes, at + 12, index);
      bytes[bitmapOffset + index] = 0x80;
    }
  }
  return bytes;
}

uint16_t varyingAdvance(uint32_t cp) { return static_cast<uint16_t>(160 + (cp % 7) * 16); }
uint16_t hangulUniformAdvance(uint32_t cp) { return cp >= HANGUL_FIRST ? 368 : varyingAdvance(cp); }

// A chapter-sized word list: every one of `distinct` syllables (shuffled by a
// prime stride; 7919 is coprime with every count used here), three per word,
// the whole set repeated `repeats` times.
std::vector<std::string> hangulWords(uint32_t distinct, uint32_t repeats, uint32_t firstSyllable = 0) {
  std::vector<std::string> words;
  std::string word;
  for (uint32_t r = 0; r < repeats; ++r) {
    for (uint32_t i = 0; i < distinct; ++i) {
      const uint32_t syllable = firstSyllable + (i * 7919u + r * 31u) % distinct;
      utf8AppendCodepoint(HANGUL_FIRST + syllable, word);
      if (word.size() >= 9) {
        words.push_back(std::move(word));
        word.clear();
      }
    }
  }
  if (!word.empty()) words.push_back(word);
  return words;
}

class SdFontAdvance : public testing::Test {
 protected:
  void SetUp() override {
    FakeSD::reset();
    FakeFontMemory::fail = false;
    fontStorage.readOpens = 0;
  }
  void install(const Bytes& bytes) { FakeSD::files["/big.cpfont"] = bytes; }
};

// Regression: the old 768-entry cap refused new entries, so a 1,600-syllable
// chapter measured every later syllable through one SD open per glyph (host:
// ~10,000 opens). Now every requested advance is resident after one batched
// pass: one open per ADVANCE_FETCH_BATCH misses.
TEST_F(SdFontAdvance, LargeAlphabetRequestIsFullyResidentWithBatchedReads) {
  install(largeFont(varyingAdvance));  // non-uniform: exercises the LRU table
  SdCardFont font;
  ASSERT_TRUE(font.load("/big.cpfont"));
  constexpr uint32_t DISTINCT = 1700;
  const auto words = hangulWords(DISTINCT, 2);
  fontStorage.readOpens = 0;
  EXPECT_EQ(font.buildAdvanceTable(words, false, 0x01), 0);
  const unsigned batches = (DISTINCT + 1 + SdCardFont::ADVANCE_FETCH_BATCH - 1) / SdCardFont::ADVANCE_FETCH_BATCH;
  EXPECT_LE(fontStorage.readOpens, batches);

  const unsigned opensBeforeMeasure = fontStorage.readOpens;
  for (uint32_t i = 0; i < DISTINCT; ++i) {
    uint16_t advance = 0;
    ASSERT_TRUE(font.lookupAdvance(HANGUL_FIRST + i, 0, &advance)) << i;
    EXPECT_EQ(advance, varyingAdvance(HANGUL_FIRST + i));
  }
  EXPECT_EQ(fontStorage.readOpens, opensBeforeMeasure);  // lookups are RAM only
  EXPECT_LE(font.advanceCacheCapacity(0), SdCardFont::ADVANCE_REQUEST_LIMIT);

  // The request that needed more than the steady-state bound is over once a
  // page renders: the table shrinks back and keeps the most recent entries.
  font.clearCache();
  EXPECT_LE(font.advanceCacheCapacity(0), SdCardFont::ADVANCE_CACHE_LIMIT);
  EXPECT_LE(font.advanceCacheSize(0), SdCardFont::ADVANCE_CACHE_LIMIT);
}

TEST_F(SdFontAdvance, LeastRecentlyRequestedEntriesAreEvictedFirst) {
  install(largeFont(varyingAdvance));
  SdCardFont font;
  ASSERT_TRUE(font.load("/big.cpfont"));
  const auto first = hangulWords(500, 1, 0);
  const auto second = hangulWords(500, 1, 500);
  const auto third = hangulWords(500, 1, 1000);
  ASSERT_EQ(font.buildAdvanceTable(first, false, 0x01), 0);
  ASSERT_EQ(font.buildAdvanceTable(second, false, 0x01), 0);
  // Re-request the first set: it becomes the most recent.
  ASSERT_EQ(font.buildAdvanceTable(first, false, 0x01), 0);
  ASSERT_EQ(font.buildAdvanceTable(third, false, 0x01), 0);
  EXPECT_LE(font.advanceCacheSize(0), SdCardFont::ADVANCE_CACHE_LIMIT);
  unsigned firstResident = 0, secondResident = 0;
  for (uint32_t i = 0; i < 500; ++i) {
    firstResident += font.lookupAdvance(HANGUL_FIRST + i, 0, nullptr);
    secondResident += font.lookupAdvance(HANGUL_FIRST + 500 + i, 0, nullptr);
    ASSERT_TRUE(font.lookupAdvance(HANGUL_FIRST + 1000 + i, 0, nullptr)) << "current request evicted";
  }
  EXPECT_EQ(secondResident, 0u);  // oldest generation went first
  // Only as many as needed were evicted: most of the re-requested set survived.
  EXPECT_GT(firstResident, 200u);
  EXPECT_LE(firstResident, SdCardFont::ADVANCE_CACHE_LIMIT - 500u);
  EXPECT_GT(font.getStats().advanceEvictions, 0u);
}

// Hangul syllables and CJK ideographs usually share one advance: one
// sequential scan replaces per-glyph metric reads for the whole block.
TEST_F(SdFontAdvance, UniformIntervalIsScannedOnceAndServesEveryCodepoint) {
  install(largeFont(hangulUniformAdvance));
  SdCardFont font;
  ASSERT_TRUE(font.load("/big.cpfont"));
  fontStorage.readOpens = 0;
  EXPECT_EQ(font.buildAdvanceTable(hangulWords(300, 1), true, 0x01), 0);
  EXPECT_LE(fontStorage.readOpens, 2u);  // the scanning batch, then space and hyphen
  EXPECT_EQ(font.getStats().uniformScans, 1u);
  EXPECT_LT(font.advanceCacheSize(0), 8u);  // only space and hyphen needed entries
  for (uint32_t i = 0; i < HANGUL_COUNT; ++i) {
    EXPECT_EQ(font.getAdvance(HANGUL_FIRST + i, 0), 368) << i;
  }
  fontStorage.readOpens = 0;
  EXPECT_EQ(font.buildAdvanceTable(hangulWords(1500, 1, 700), false, 0x01), 0);
  EXPECT_EQ(fontStorage.readOpens, 0u);
  EXPECT_FALSE(font.lookupAdvance(HANGUL_FIRST + HANGUL_COUNT, 0, nullptr));  // outside the interval
}

TEST_F(SdFontAdvance, NonUniformIntervalIsTriedOnceThenCachedPerGlyph) {
  install(largeFont(varyingAdvance));
  SdCardFont font;
  ASSERT_TRUE(font.load("/big.cpfont"));
  ASSERT_EQ(font.buildAdvanceTable(hangulWords(300, 1), false, 0x01), 0);
  ASSERT_EQ(font.buildAdvanceTable(hangulWords(300, 1, 1000), false, 0x01), 0);
  EXPECT_EQ(font.getStats().uniformScans, 1u);  // rejected after a few runs, never retried
  EXPECT_EQ(font.getAdvance(HANGUL_FIRST + 1003, 0), varyingAdvance(HANGUL_FIRST + 1003));
}

TEST_F(SdFontAdvance, InvisibleCodepointsAreNeitherFetchedNorCountedMissing) {
  install(largeFont(varyingAdvance));
  SdCardFont font;
  ASSERT_TRUE(font.load("/big.cpfont"));
  // ZWJ, variation selector 16, skin tone and a tag character are absent from
  // the font but carry no width; only the emoji itself is reported missing.
  EXPECT_EQ(font.buildAdvanceTable("A‍️\U0001F3FD\U000E0067B\U0001F469", 0x01), 1);
  EXPECT_TRUE(font.lookupAdvance('A', 0, nullptr));
  EXPECT_TRUE(font.lookupAdvance('B', 0, nullptr));
  EXPECT_FALSE(font.lookupAdvance(0x200D, 0, nullptr));
}

TEST_F(SdFontAdvance, CodepointArrayRequestAndCoverageCheckUseResidentIntervals) {
  install(largeFont(varyingAdvance));
  SdCardFont font;
  ASSERT_TRUE(font.load("/big.cpfont"));
  const uint32_t cps[] = {'Q', 0x1F4BB, HANGUL_FIRST + 5};
  fontStorage.readOpens = 0;
  EXPECT_EQ(font.buildAdvanceTable(cps, 3, 0x01), 1);
  EXPECT_EQ(fontStorage.readOpens, 1u);
  fontStorage.readOpens = 0;
  EXPECT_TRUE(font.hasGlyph('Q', 0));
  EXPECT_FALSE(font.hasGlyph(0x1F4BB, 0));
  EXPECT_EQ(fontStorage.readOpens, 0u);
}

// BoundedUI keeps intervals on SD. A repeated unsupported symbol must not
// reopen the font for every occurrence.
TEST_F(SdFontAdvance, BoundedModeRemembersRecentMisses) {
  install(largeFont(varyingAdvance));
  SdCardFont font;
  ASSERT_TRUE(font.load("/big.cpfont", SdCardFont::LoadMode::BoundedUI));
  fontStorage.readOpens = 0;
  EXPECT_EQ(font.getEpdFont()->getGlyph(0x1F4BB), nullptr);
  const unsigned afterFirst = fontStorage.readOpens;
  EXPECT_GT(afterFirst, 0u);
  for (int i = 0; i < 20; ++i) {
    EXPECT_EQ(font.getEpdFont()->getGlyph(0x1F4BB), nullptr);
    EXPECT_FALSE(font.hasGlyph(0x1F4BB, 0));
  }
  EXPECT_EQ(fontStorage.readOpens, afterFirst);
  EXPECT_FALSE(font.boundedReadFailed());
  ASSERT_NE(font.getEpdFont()->getGlyph('A'), nullptr);  // present glyphs are unaffected
}
}  // namespace
