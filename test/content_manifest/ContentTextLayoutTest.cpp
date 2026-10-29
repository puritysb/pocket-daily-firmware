#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "pocket_daily/ContentTextLayout.h"

namespace {
using namespace PocketDaily::Content;
struct Output {
  std::vector<std::string> lines;
  unsigned measureCalls = 0, measuredCodepoints = 0;
  static int measure(void* self, const char* text) {
    auto& out = *static_cast<Output*>(self);
    int width = 0;
    while (*text)
      if ((static_cast<unsigned char>(*text++) & 0xC0) != 0x80) ++width;
    ++out.measureCalls;
    out.measuredCodepoints += width;
    return width;
  }
  TextPainter painter() {
    return {this, measure,
            [](void* self, const char* line, unsigned) { static_cast<Output*>(self)->lines.emplace_back(line); }};
  }
};

// Deliberately nonuniform metrics and a shrinking ligature: f alone is wider
// than fi. A sum of character widths or monotone-prefix search is not a shaper.
int shapedWidth(void* self, const char* text) {
  Output::measure(self, text);
  int width = 0;
  for (; *text; ++text) {
    if (text[0] == 'f' && text[1] == 'i') {
      width += 4;
      ++text;
    } else {
      switch (*text) {
        case 'f':
          width += 7;
          break;
        case 'a':
          width += 3;
          break;
        case 'z':
          width += 5;
          break;
        case ' ':
          width += 1;
          break;
        default:
          width += 2;
          break;
      }
    }
  }
  return width;
}
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

TEST(ContentTextLayout, OrdinaryStarterProseKeepsWordBreaks) {
  Output out;
  const auto result =
      drawWrappedText("Open your book. Read one page, then decide whether to keep going.", 20, 6, out.painter());
  EXPECT_EQ(out.lines,
            (std::vector<std::string>{"Open your book. Read", "one page, then", "decide whether to", "keep going."}));
  EXPECT_EQ(result.lines, 4u);
  EXPECT_FALSE(result.truncated);
}

TEST(ContentTextLayout, CompleteWordsMayFitEvenWhenShapedPrefixesDoNot) {
  for (const auto& [text, width] : {std::pair{"fi", 5}, std::pair{"a fi", 8}}) {
    SCOPED_TRACE(text);
    Output out;
    auto painter = out.painter();
    painter.measure = shapedWidth;
    const auto result = drawWrappedText(text, width, 3, painter);
    EXPECT_EQ(out.lines, (std::vector<std::string>{text}));
    EXPECT_EQ(result.lines, 1u);
    EXPECT_FALSE(result.truncated);
  }
}

TEST(ContentTextLayout, OverwideWholeWordRetainsFirstOverflowPrefixFallback) {
  Output out;
  auto painter = out.painter();
  painter.measure = shapedWidth;
  // fizz does not fit. The fallback must stop at f, even though fi is narrower.
  const auto result = drawWrappedText("fizz", 5, 3, painter);
  EXPECT_TRUE(result.truncated);
  EXPECT_EQ(result.lines, 0u);
  EXPECT_TRUE(out.lines.empty());
}

TEST(ContentTextLayout, MeasuresWordBoundariesInsteadOfEveryGrowingPrefix) {
  Output out;
  const std::string text = "Read one page, then decide whether to keep going.";
  const auto result = drawWrappedText(text.c_str(), 100, 2, out.painter());
  EXPECT_EQ(out.lines, (std::vector<std::string>{text}));
  EXPECT_FALSE(result.truncated);
  // Nine words and their separators. Count scanned text as well as calls:
  // nine full-line probes would be very different work from nine short ones.
  // Restoring the old per-character loop violates both budgets.
  EXPECT_LE(out.measureCalls, 18u);
  EXPECT_LE(out.measuredCodepoints, text.size() * 10);
}

TEST(ContentTextLayout, SpacesTabsAndHardBreaksKeepTheirExistingOutput) {
  Output out;
  const auto result = drawWrappedText(" \tAlpha\tbeta\r\n\r\n한글\tかな\nlast\t", 30, 6, out.painter());
  EXPECT_EQ(out.lines, (std::vector<std::string>{"Alpha beta", "한글 かな", "last "}));
  EXPECT_EQ(result.lines, 3u);
  EXPECT_FALSE(result.truncated);
  out.lines.clear();
  drawWrappedText("one  two", 5, 3, out.painter());
  EXPECT_EQ(out.lines, (std::vector<std::string>{"one ", "two"}));
}

TEST(ContentTextLayout, OverwideMultibyteWordsSplitAtWholeCodepoints) {
  Output out;
  const auto result = drawWrappedText("가나다라마바사\nA😀B😀C", 3, 6, out.painter());
  EXPECT_EQ(out.lines, (std::vector<std::string>{"가나다", "라마바", "사", "A😀B", "😀C"}));
  EXPECT_EQ(result.lines, 5u);
  EXPECT_FALSE(result.truncated);
}

TEST(ContentTextLayout, EllipsisStillFitsOrLeavesAnEmptyLineWhenTooNarrow) {
  for (const auto& [width, expected] : {std::pair{2, ""}, std::pair{3, "..."}, std::pair{6, "가나다..."}}) {
    SCOPED_TRACE(width);
    Output out;
    const auto result = drawWrappedText("가나다라마바사", width, 1, out.painter());
    EXPECT_EQ(out.lines, (std::vector<std::string>{expected}));
    EXPECT_EQ(result.lines, 1u);
    EXPECT_TRUE(result.truncated);
  }
}

TEST(ContentTextLayout, ScratchLimitPreservesWholeWordsAndMultibyteBoundaries) {
  for (const auto& text : {std::string(124, 'a') + "b", std::string(123, 'a') + "가"}) {
    Output out;
    const auto result = drawWrappedText(text.c_str(), 1000, 3, out.painter());
    ASSERT_EQ(out.lines.size(), 2u);
    EXPECT_EQ(out.lines[0] + out.lines[1], text);
    EXPECT_EQ(out.lines[0], text.substr(0, text[123] == 'a' ? 124 : 123));
    EXPECT_FALSE(result.truncated);
  }
  Output out;
  const std::string word(119, 'a');
  const auto result = drawWrappedText((word + " 가나다").c_str(), 1000, 3, out.painter());
  EXPECT_EQ(out.lines, (std::vector<std::string>{word, "가나다"}));
  EXPECT_FALSE(result.truncated);
}

TEST(ContentTextLayout, ScratchLimitReservesAllThreeEllipsisBytesAndTerminator) {
  Output out;
  const std::string text(125, 'a');
  const auto result = drawWrappedText(text.c_str(), 1000, 1, out.painter());
  EXPECT_EQ(out.lines, (std::vector<std::string>{std::string(124, 'a') + "..."}));
  EXPECT_TRUE(result.truncated);
  EXPECT_EQ(result.lines, 1u);
}
}  // namespace
