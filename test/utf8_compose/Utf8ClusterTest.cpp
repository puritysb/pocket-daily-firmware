#include <Utf8.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace {
// Returns the codepoints left for a renderer after skipping invisible
// codepoints and emoji cluster tails, the same walk GfxRenderer::drawText does.
std::u32string renderedBases(const char* text) {
  std::u32string out;
  const auto* p = reinterpret_cast<const unsigned char*>(text);
  while (*p) {
    const uint32_t cp = utf8NextCodepoint(&p);
    if (utf8IsInvisible(cp)) continue;
    out.push_back(cp);
    if (!utf8IsCombiningMark(cp)) utf8SkipEmojiClusterTail(&p, cp);
  }
  return out;
}
}  // namespace

TEST(Utf8Cluster, DefaultIgnorableRanges) {
  for (uint32_t cp : {0x00ADu, 0x034Fu,  0x061Cu,  0x115Fu,  0x1160u,  0x180Eu,  0x200Bu,  0x200Cu, 0x200Du,
                      0x200Eu, 0x202Au,  0x2060u,  0x2066u,  0x206Fu,  0x3164u,  0xFE00u,  0xFE0Fu, 0xFEFFu,
                      0xFFA0u, 0x1BCA0u, 0x1D173u, 0xE0001u, 0xE0020u, 0xE007Fu, 0xE0100u, 0xE01EFu}) {
    EXPECT_TRUE(utf8IsDefaultIgnorable(cp)) << std::hex << cp;
  }
  for (uint32_t cp :
       {0x20u, 0x41u, 0xACu, 0xAEu, 0x2010u, 0x2070u, 0x3000u, 0xAC00u, 0xFE10u, 0xFFFDu, 0x1F469u, 0x1F3FBu}) {
    EXPECT_FALSE(utf8IsDefaultIgnorable(cp)) << std::hex << cp;
  }
  EXPECT_TRUE(utf8IsInvisible(0x1F3FB));
  EXPECT_TRUE(utf8IsInvisible(0x1F3FF));
  EXPECT_FALSE(utf8IsInvisible(0x1F400));
}

TEST(Utf8Cluster, EmojiBases) {
  for (uint32_t cp : {0x00A9u, 0x2194u, 0x2600u, 0x2705u, 0x2B50u, 0x1F1F0u, 0x1F469u, 0x1F4BBu, 0x1FAE8u}) {
    EXPECT_TRUE(utf8IsEmojiBase(cp)) << std::hex << cp;
  }
  for (uint32_t cp : {0x41u, 0xAC00u, 0x2013u, 0x1F3FBu, 0x200Du}) {
    EXPECT_FALSE(utf8IsEmojiBase(cp)) << std::hex << cp;
  }
}

TEST(Utf8Cluster, ZwjSequenceCollapsesToItsFirstPictograph) {
  // woman + medium skin tone + ZWJ + laptop, then Hangul.
  EXPECT_EQ(renderedBases("\U0001F469\U0001F3FD‍\U0001F4BB가"), U"\U0001F469가");
  // family: man ZWJ woman ZWJ girl ZWJ boy
  EXPECT_EQ(renderedBases("\U0001F468‍\U0001F469‍\U0001F467‍\U0001F466!"), U"\U0001F468!");
  // rainbow flag: white flag VS16 ZWJ rainbow
  EXPECT_EQ(renderedBases("\U0001F3F3️‍\U0001F308"), U"\U0001F3F3");
  // subdivision flag: black flag + tag sequence
  EXPECT_EQ(renderedBases("\U0001F3F4\U000E0067\U000E0062\U000E0065\U000E006E\U000E0067\U000E007Fx"), U"\U0001F3F4x");
  // regional indicator pair is one flag; a third indicator starts a new one
  EXPECT_EQ(renderedBases("\U0001F1F0\U0001F1F7\U0001F1EF"), U"\U0001F1F0\U0001F1EF");
}

TEST(Utf8Cluster, JoinersOutsideEmojiAreOnlyInvisible) {
  EXPECT_EQ(renderedBases("a‍b"), U"ab");
  EXPECT_EQ(renderedBases("क्‍ष"), U"क्ष");
  // A ZWJ after an emoji that is not followed by a pictograph does not swallow text.
  EXPECT_EQ(renderedBases("\U0001F469‍가"), U"\U0001F469가");
  // Keycap: the base digit stays, VS16 is invisible, the keycap mark is kept for the caller.
  EXPECT_EQ(renderedBases("1️⃣"), U"1⃣");
  // A stray skin tone and a trailing ZWJ at the end of the string.
  EXPECT_EQ(renderedBases("\U0001F3FDx\U0001F469‍"), U"x\U0001F469");
}
