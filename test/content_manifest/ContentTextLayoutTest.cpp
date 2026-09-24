#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "pocket_daily/ContentTextLayout.h"

namespace {
using namespace PocketDaily::Content;
struct Output {
  std::vector<std::string> lines;
  TextPainter painter() {
    return {this,
            [](void*, const char* text) {
              int width = 0;
              while (*text)
                if ((static_cast<unsigned char>(*text++) & 0xC0) != 0x80) ++width;
              return width;
            },
            [](void* self, const char* line, unsigned) { static_cast<Output*>(self)->lines.emplace_back(line); }};
  }
};
TEST(ContentTextLayout, WrapsWordsNewlinesAndKoreanWithoutBreakingUtf8) {
  Output out;
  const auto result = drawWrappedText("한글 세 글자\nNext word", 5, 5, out.painter());
  EXPECT_EQ(out.lines, (std::vector<std::string>{"한글 세", "글자", "Next", "word"}));
  EXPECT_EQ(result.lines, 4u);
  EXPECT_FALSE(result.truncated);
}

TEST(ContentTextLayout, CoverageNormalizesWhitespaceAndPreservesUtf8AcrossScratchBoundaries) {
  std::string text(126, 'a');
  text += "한글\r\n\tEnd";
  std::string seen;
  EXPECT_TRUE(checkLayoutText(text.c_str(), &seen, [](void* context, const char* chunk) {
    const std::string part(chunk);
    EXPECT_LT(part.size(), 128U);
    EXPECT_NE(static_cast<unsigned char>(part[0]) & 0xC0, 0x80);
    *static_cast<std::string*>(context) += part;
    return true;
  }));
  EXPECT_EQ(seen, std::string(126, 'a') + "한글   End");
  EXPECT_FALSE(checkLayoutText("missing", nullptr, [](void*, const char*) { return false; }));
  EXPECT_TRUE(checkLayoutText("", nullptr, [](void*, const char*) {
    ADD_FAILURE();
    return false;
  }));
  Output out;
  drawWrappedText("A\tB\nC", 10, 3, out.painter());
  EXPECT_EQ(out.lines, (std::vector<std::string>{"A B", "C"}));
}
TEST(ContentTextLayout, LastLineEllipsisFitsAndLongWordsMakeProgress) {
  Output out;
  const auto result = drawWrappedText("abcdefghij", 5, 1, out.painter());
  EXPECT_EQ(out.lines, (std::vector<std::string>{"ab..."}));
  EXPECT_TRUE(result.truncated);
  out.lines.clear();
  drawWrappedText("가나다라마바사", 4, 1, out.painter());
  EXPECT_EQ(out.lines, (std::vector<std::string>{"가..."}));
}
TEST(ContentTextLayout, FixedScratchSplitsWideTextWithoutLosingBytes) {
  Output out;
  const std::string text(300, 'a');
  const auto result = drawWrappedText(text.c_str(), 1000, 8, out.painter());
  std::string joined;
  for (const auto& line : out.lines) {
    EXPECT_LT(line.size(), 128u);
    joined += line;
  }
  EXPECT_EQ(joined, text);
  EXPECT_FALSE(result.truncated);
}
TEST(ContentTextLayout, EmptyInvalidGeometryAndTooWideGlyphDoNotOverdraw) {
  Output out;
  EXPECT_EQ(drawWrappedText(" \n\t", 10, 2, out.painter()).lines, 0u);
  EXPECT_EQ(drawWrappedText("x", 0, 2, out.painter()).lines, 0u);
  auto painter = out.painter();
  painter.measure = [](void*, const char*) { return 10; };
  EXPECT_TRUE(drawWrappedText("한", 5, 2, painter).truncated);
  EXPECT_TRUE(out.lines.empty());
}
}  // namespace
