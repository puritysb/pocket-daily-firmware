#include <Utf8.h>
#include <gtest/gtest.h>

#include <string>

#include "Epub/hyphenation/Hyphenator.h"

namespace {
uint32_t codepointAt(const std::string& word, size_t offset) {
  const auto* p = reinterpret_cast<const unsigned char*>(word.c_str() + offset);
  return utf8NextCodepoint(&p);
}
uint32_t codepointBefore(const std::string& word, size_t offset) {
  size_t lead = offset - 1;
  while (lead > 0 && (static_cast<uint8_t>(word[lead]) & 0xC0) == 0x80) --lead;
  return codepointAt(word, lead);
}
}  // namespace

// Fallback breaks (used when an overlong word must be split) never separate an
// emoji from its modifiers or joined components, nor strand a joiner or
// variation selector at a line start.
TEST(HyphenatorCluster, FallbackBreaksKeepEmojiClustersWhole) {
  Hyphenator::setPreferredLanguage("");
  std::string word = "abcdef";
  utf8AppendCodepoint(0x1F469, word);
  utf8AppendCodepoint(0x1F3FD, word);
  utf8AppendCodepoint(0x200D, word);
  utf8AppendCodepoint(0x1F4BB, word);
  utf8AppendCodepoint(0xFE0F, word);
  word += "ghijkl";
  const auto breaks = Hyphenator::breakOffsets(word, true);
  ASSERT_FALSE(breaks.empty());
  for (const auto& info : breaks) {
    const uint32_t after = codepointAt(word, info.byteOffset);
    EXPECT_FALSE(utf8IsInvisible(after)) << info.byteOffset;
    EXPECT_NE(codepointBefore(word, info.byteOffset), 0x200Du) << info.byteOffset;
  }
}
