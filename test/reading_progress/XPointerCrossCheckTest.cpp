// Cross-compatibility of reading positions between the Pocket Daily app and the
// reader firmware (docs/reading-progress-v1.md).
//
// app -> reader: every XPointer the app's reader engine produced for a real EPUB
// (fixtures/app-xpointers-*.json, with the next source characters in `textAt`)
// must resolve, through the same ProgressMapper step toCrossPoint() uses, to the
// visible character where `textAt` starts.
//
// reader -> app: XPointers the firmware generates for sample positions are written
// to <build>/reading_progress/firmware-xpointers.json and must match the committed
// fixtures/firmware-xpointers.json, which the app verifies with its own resolver.
#include <ArduinoJson.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "ChapterXPathResolver.h"
#include "Epub.h"
#include "Epub/htmlEntities.h"
#include "ProgressMapper.h"

namespace {

std::string readFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}

size_t utf8Length(unsigned char lead) {
  if (lead < 0x80) return 1;
  if ((lead & 0xE0) == 0xC0) return 2;
  if ((lead & 0xF0) == 0xE0) return 3;
  if ((lead & 0xF8) == 0xF0) return 4;
  return 1;
}

void appendCodepoint(uint32_t cp, std::string& out) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

std::vector<std::string> splitCodepoints(const std::string& text) {
  std::vector<std::string> out;
  for (size_t i = 0; i < text.size();) {
    const size_t n = std::min(utf8Length(static_cast<unsigned char>(text[i])), text.size() - i);
    out.push_back(text.substr(i, n));
    i += n;
  }
  return out;
}

bool nonVisibleTag(std::string name) {
  for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return name == "head" || name == "style" || name == "script" || name == "title";
}

struct VisibleText {
  std::vector<std::string> chars;                     // one UTF-8 string per codepoint
  std::vector<bool> inParagraph;                      // inside a <p> or <li>
  std::vector<std::pair<size_t, size_t>> paragraphs;  // [start, end) of each <p>, in order
  size_t bodyStart = 0;                               // visible characters before <body>'s text
  size_t bodyEnd = 0;                                 // visible characters up to </body>
  size_t size() const { return chars.size(); }
};

// Independent reference for "visible text" order: the XHTML's character data as a
// DOM would hold it (markup and head/style/script/title removed, entities decoded,
// CRLF/CR read as LF), plus which characters sit inside <p>/<li>.
VisibleText visibleText(const std::string& xhtml) {
  VisibleText result;
  std::vector<std::string>& out = result.chars;
  std::vector<std::string> open;  // lower-case names of open visible elements
  std::vector<size_t> openParagraphs;
  int paragraphOrItem = 0;
  auto push = [&](std::string cp) {
    out.push_back(std::move(cp));
    result.inParagraph.push_back(paragraphOrItem > 0);
  };
  int hidden = 0;
  bool lastCr = false;
  for (size_t i = 0; i < xhtml.size();) {
    const char c = xhtml[i];
    if (c == '<') {
      const size_t end = xhtml.find('>', i);
      if (end == std::string::npos) break;
      const std::string tag = xhtml.substr(i + 1, end - i - 1);
      i = end + 1;
      if (tag.empty() || tag[0] == '!' || tag[0] == '?') continue;
      const bool close = tag[0] == '/';
      const bool selfClose = tag.back() == '/';
      const size_t nameStart = close ? 1 : 0;
      const size_t nameEnd = tag.find_first_of(" \t\r\n/", nameStart);
      const std::string name =
          tag.substr(nameStart, nameEnd == std::string::npos ? std::string::npos : nameEnd - nameStart);
      std::string lower = name;
      for (auto& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      lastCr = false;
      if (hidden > 0) {
        if (close)
          --hidden;
        else if (!selfClose)
          ++hidden;
      } else if (!close && !selfClose && nonVisibleTag(name)) {
        hidden = 1;
      } else if (!close && !selfClose) {
        open.push_back(lower);
        if (lower == "p" || lower == "li") ++paragraphOrItem;
        if (lower == "body") result.bodyStart = out.size();
        if (lower == "p") {
          openParagraphs.push_back(result.paragraphs.size());
          result.paragraphs.emplace_back(out.size(), out.size());
        }
      } else if (close && !open.empty()) {
        if (open.back() == "body") result.bodyEnd = out.size();
        if (open.back() == "p" || open.back() == "li") --paragraphOrItem;
        if (open.back() == "p" && !openParagraphs.empty()) {
          result.paragraphs[openParagraphs.back()].second = out.size();
          openParagraphs.pop_back();
        }
        open.pop_back();
      }
      continue;
    }
    if (hidden > 0) {
      ++i;
      continue;
    }
    if (c == '\n' && lastCr) {
      lastCr = false;
      ++i;
      continue;
    }
    lastCr = c == '\r';
    if (c == '\r') {
      push("\n");
      ++i;
      continue;
    }
    if (c == '&') {
      const size_t semi = xhtml.find(';', i);
      if (semi != std::string::npos && semi - i < 16) {
        const std::string entity = xhtml.substr(i, semi - i + 1);
        std::string decoded;
        if (entity.size() > 3 && entity[1] == '#') {
          const bool hex = entity[2] == 'x' || entity[2] == 'X';
          appendCodepoint(static_cast<uint32_t>(std::stoul(entity.substr(hex ? 3 : 2), nullptr, hex ? 16 : 10)),
                          decoded);
        } else if (const char* named = lookupHtmlEntity(entity.c_str(), entity.size())) {
          decoded = named;
        }
        if (!decoded.empty()) {
          for (auto& cp : splitCodepoints(decoded)) push(cp);
          i = semi + 1;
          continue;
        }
      }
    }
    const size_t n = std::min(utf8Length(static_cast<unsigned char>(c)), xhtml.size() - i);
    push(xhtml.substr(i, n));
    i += n;
  }
  return result;
}

std::string textFrom(const VisibleText& text, size_t at, size_t count) {
  std::string out;
  for (size_t i = at; i < text.size() && i < at + count; ++i) out += text.chars[i];
  return out;
}

struct Book {
  std::shared_ptr<Epub> epub;
  std::vector<VisibleText> text;  // visible text per spine item
};

Book openBook(const std::string& file) {
  Book book;
  book.epub = std::make_shared<Epub>(std::string(READING_PROGRESS_FIXTURES) + "/" + file);
  if (!book.epub->load()) return {};
  for (int i = 0; i < book.epub->getSpineItemsCount(); ++i) {
    std::string xhtml;
    book.epub->readItem(book.epub->getSpineItem(i).href, xhtml);
    book.text.push_back(visibleText(xhtml));
  }
  return book;
}

int spineOf(const std::string& xpointer) {
  int fragment = 0;
  return std::sscanf(xpointer.c_str(), "/body/DocFragment[%d]", &fragment) == 1 ? fragment - 1 : -1;
}

struct CrossCheck {
  int exact = 0;
  int coarser = 0;
  std::vector<std::string> failures;
};

CrossCheck crossCheck(const char* epubFile, const char* fixtureFile) {
  CrossCheck result;
  const Book book = openBook(epubFile);
  if (!book.epub) {
    result.failures.push_back("could not open EPUB");
    return result;
  }
  JsonDocument fixture;
  if (deserializeJson(fixture, readFile(std::string(READING_PROGRESS_FIXTURES) + "/" + fixtureFile))) {
    result.failures.push_back("could not parse fixture");
    return result;
  }
  for (JsonObject position : fixture["positions"].as<JsonArray>()) {
    const std::string xpointer = position["xpointer"].as<std::string>();
    const std::string expected = position["textAt"].as<std::string>();
    const int spine = spineOf(xpointer);
    if (spine != position["sectionIndex"].as<int>()) {
      result.failures.push_back(xpointer + ": wrong DocFragment");
      continue;
    }
    const XPathSpineTarget target = ProgressMapper::locateInSpine(book.epub, spine, xpointer);
    if (!target.found) {
      result.failures.push_back(xpointer + ": not resolved");
      continue;
    }
    const auto& text = book.text[spine];
    if (target.totalVisibleChars != text.size()) {
      result.failures.push_back(xpointer + ": visible text length differs from reference");
      continue;
    }
    const size_t expectedCount = splitCodepoints(expected).size();
    const std::string actual = textFrom(text, target.visibleChar, expectedCount);
    if (actual == expected) {
      ++result.exact;
      continue;
    }
    // Coarser: the expected text starts later in the same spine item (element-level landing).
    std::ostringstream detail;
    detail << xpointer << ": expected '" << expected << "' got '" << actual << "' at " << target.visibleChar;
    ++result.coarser;
    result.failures.push_back(detail.str());
  }
  return result;
}

TEST(ReadingProgressXPointer, KoreanSampleAppPositionsResolveExactly) {
  const auto result = crossCheck("pocket-daily-epub-check.epub", "app-xpointers-korean.json");
  for (const auto& failure : result.failures) ADD_FAILURE() << failure;
  EXPECT_EQ(result.exact, 15);
}

TEST(ReadingProgressXPointer, FrankensteinAppPositionsResolveExactly) {
  const auto result = crossCheck("frankenstein-se.epub", "app-xpointers-frankenstein.json");
  for (const auto& failure : result.failures) ADD_FAILURE() << failure;
  EXPECT_EQ(result.exact, 114);
}

// Edge cases of text()[N].offset on synthetic chapters: where the XPointer lands,
// as the visible text that follows it.
std::string chapter(const std::string& body) {
  // The XHTML DOCTYPE makes expat hand HTML named entities (&nbsp;) to the default
  // handler, as it does for real EPUB chapters.
  return "<?xml version=\"1.0\"?>\n<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.1//EN\" "
         "\"http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd\">\n"
         "<html><head><title>T</title><style>p{}</style></head><body>" +
         body + "</body></html>";
}

std::string landing(const std::string& body, const std::string& xpointer) {
  const std::string xhtml = chapter(body);
  const auto epub = Epub::fromSpine({xhtml});
  const XPathSpineTarget target = ProgressMapper::locateInSpine(epub, 0, xpointer);
  if (!target.found) return "<not found>";
  return textFrom(visibleText(xhtml), target.visibleChar, 6);
}

TEST(ReadingProgressXPointer, TextNodesSkipWhitespaceOnlyRunsAndNestedText) {
  // Runs split by children; the empty run before <i> and the space-only run are not numbered.
  const std::string cite = "<p><i>Paradise</i>, <span>X</span>, 743</p>";
  EXPECT_EQ(landing(cite, "/body/DocFragment[1]/body/p/text()[1].0"), ", X, 7");
  EXPECT_EQ(landing(cite, "/body/DocFragment[1]/body/p/text()[2].2"), "743");
  EXPECT_EQ(landing("<p><b>A</b> <b>B</b> tail</p>", "/body/DocFragment[1]/body/p/text().1"), "tail");
  // Offsets count leading whitespace of the numbered run.
  EXPECT_EQ(landing("<p>To<br/>\n\t\tWilliam</p>", "/body/DocFragment[1]/body/p/text()[2].3"), "Willia");
}

TEST(ReadingProgressXPointer, OffsetsCountCodepointsAfterEntityAndLineEndDecoding) {
  EXPECT_EQ(landing("<p>a&#8212;b&#x2014;c&amp;d</p>", "/body/DocFragment[1]/body/p/text().3"),
            "\xE2\x80\x94"
            "c&d");
  EXPECT_EQ(landing("<p>one\r\ntwo\rthree</p>", "/body/DocFragment[1]/body/p/text().8"), "three");
  EXPECT_EQ(landing("<p>\xF0\x9F\x91\xA9\xF0\x9F\x8F\xBD\xE2\x80\x8D\xF0\x9F\x92\xBB!</p>",
                    "/body/DocFragment[1]/body/p/text().4"),
            "!");
}

TEST(ReadingProgressXPointer, MissingOrShortTextNodesLandOnTheElement) {
  // Past the run's end clamps to its end; a missing run lands on the element's start.
  EXPECT_EQ(landing("<p>ab<i>cd</i>ef</p>", "/body/DocFragment[1]/body/p/text()[1].9"), "cdef");
  EXPECT_EQ(landing("<p>ab</p><p>zz</p>", "/body/DocFragment[1]/body/p[1]/text()[3].1"), "abzz");
  EXPECT_EQ(landing("<div><p>x</p></div><div><p>yz</p></div>", "/body/DocFragment[1]/body/div[2]/p"), "yz");
  // The first step is a child of <body>, not the first match at any depth.
  EXPECT_EQ(landing("<div><p>nested</p></div><p>direct</p>", "/body/DocFragment[1]/body/p/text().0"), "direct");
  EXPECT_EQ(landing("<p>ab</p>", "/body/DocFragment[1]/body/p[4]/text().1"), "<not found>");
}

// All entities count as their decoded code points on both sides: the five XML
// entities (&apos; included), numeric references and HTML named entities.
TEST(ReadingProgressXPointer, EntitiesCountAsDecodedCodepoints) {
  // i t ' s … ␠ ⍽ x ' y  (⍽ = U+00A0)
  const std::string body = "<p>it&apos;s&hellip; &nbsp;x&apos;y</p>";
  EXPECT_EQ(landing(body, "/body/DocFragment[1]/body/p/text().7"), "x'y");
  EXPECT_EQ(landing(body, "/body/DocFragment[1]/body/p/text().9"), "y");
  EXPECT_EQ(landing(body, "/body/DocFragment[1]/body/p/text().2"), "'s\xE2\x80\xA6 \xC2\xA0x");
  EXPECT_EQ(landing("<p>&quot;&lt;&gt;&amp;&#39;&#x2019;z</p>", "/body/DocFragment[1]/body/p/text().6"), "z");

  // Generated offsets count the same decoded code points.
  const auto epub = Epub::fromSpine({chapter(body)});
  const std::string byProgress = ChapterXPathResolver::findXPathForProgress(epub, 0, 0.65f);  // 7th of 10
  EXPECT_EQ(byProgress, "/body/DocFragment[1]/body/p[1]/text()[1].7");
  const std::string byParagraph = ChapterXPathResolver::findXPathForParagraphProgress(epub, 0, 1, 0.75f);
  EXPECT_EQ(byParagraph, "/body/DocFragment[1]/body/p[1]/text()[1].7");
  EXPECT_EQ(landing(body, byProgress), "x'y");
}

// Chapter text offsets (ChapterXPathResolver::findXPathForTextOffset / findTextOffsetForXPath):
// the count the layout stamps on pages -- all character data inside <body> -- reaching text
// outside <p>/<li> too, and both directions agreeing.
TEST(ReadingProgressXPointer, TextOffsetsReachEveryElementAndRoundTrip) {
  // Body text, by offset: 0 "\n" | 1-5 "Title" | 6 "\n" | 7-10 "ab\u00A0c" | 11-12 "de" |
  // 13-15 " fg" | 16 "\n" | 17-22 "loose " | 23-26 "bold" | 27-31 " tail" | 32-37 "tail2\n".
  const auto epub =
      Epub::fromSpine({chapter("\n<h1>Title</h1>\n<p>ab&nbsp;c<i>de</i> fg</p>\n"
                               "<div>loose <b>bold</b> tail</div>tail2\n")});
  struct Case {
    uint32_t offset;
    const char* xpointer;
    const char* textAt;
  };
  const Case cases[] = {
      {1, "/body/DocFragment[1]/body/h1[1]/text()[1].0", "Title\n"},
      {3, "/body/DocFragment[1]/body/h1[1]/text()[1].2", "tle\nab"},
      {7, "/body/DocFragment[1]/body/p[1]/text()[1].0",
       "ab\xC2\xA0"
       "cde"},
      {10, "/body/DocFragment[1]/body/p[1]/text()[1].3", "cde fg"},
      {11, "/body/DocFragment[1]/body/p[1]/i[1]/text()[1].0", "de fg\n"},
      {14, "/body/DocFragment[1]/body/p[1]/text()[2].1", "fg\nloo"},
      {23, "/body/DocFragment[1]/body/div[1]/b[1]/text()[1].0", "bold t"},
      {28, "/body/DocFragment[1]/body/div[1]/text()[2].1", "tailta"},
      {32, "/body/DocFragment[1]/body/text()[1].0", "tail2\n"},
  };
  for (const auto& c : cases) {
    EXPECT_EQ(ChapterXPathResolver::findXPathForTextOffset(epub, 0, c.offset), c.xpointer) << c.offset;
    uint32_t back = 0;
    EXPECT_TRUE(ChapterXPathResolver::findTextOffsetForXPath(epub, 0, c.xpointer, back)) << c.xpointer;
    EXPECT_EQ(back, c.offset) << c.xpointer;
    // ProgressMapper's streamer reads the same XPointer to the same text. It has no form for a
    // run directly in <body> (the app's xpointer.js resolves that step generically).
    if (std::string(c.xpointer).find("/body/text()") != std::string::npos) continue;
    EXPECT_EQ(
        landing("\n<h1>Title</h1>\n<p>ab&nbsp;c<i>de</i> fg</p>\n<div>loose <b>bold</b> tail</div>tail2\n", c.xpointer),
        c.textAt)
        << c.xpointer;
  }
  // An offset in a whitespace-only run (not numbered) moves to the next character.
  EXPECT_EQ(ChapterXPathResolver::findXPathForTextOffset(epub, 0, 0), "/body/DocFragment[1]/body/h1[1]/text()[1].0");
  EXPECT_EQ(ChapterXPathResolver::findXPathForTextOffset(epub, 0, 16), "/body/DocFragment[1]/body/div[1]/text()[1].0");
  // Past the text: nothing.
  EXPECT_EQ(ChapterXPathResolver::findXPathForTextOffset(epub, 0, 38), "");

  // The app's forms: omitted [1], element pointers, clamping, missing runs and elements.
  const auto offsetOf = [&](const char* xpointer) -> int64_t {
    uint32_t offset = 0;
    return ChapterXPathResolver::findTextOffsetForXPath(epub, 0, xpointer, offset) ? static_cast<int64_t>(offset) : -1;
  };
  EXPECT_EQ(offsetOf("/body/DocFragment[1]/body/p/text().1"), 8);
  EXPECT_EQ(offsetOf("/body/DocFragment[1]/body/div"), 17);
  EXPECT_EQ(offsetOf("/body/DocFragment[1]/body/p[1].0"), 7);
  EXPECT_EQ(offsetOf("/body/DocFragment[1]/body/p/text()[1].99"), 11);  // clamps to the run's end
  EXPECT_EQ(offsetOf("/body/DocFragment[1]/body/p/text()[5].0"), 7);    // no such run: the element
  EXPECT_EQ(offsetOf("/body/DocFragment[1]/body"), 0);
  EXPECT_EQ(offsetOf("/body/DocFragment[1]"), 0);
  EXPECT_EQ(offsetOf("/body/DocFragment[1]/body/p[3]"), -1);
  EXPECT_EQ(offsetOf("/body/DocFragment[1]/body/p[1]/text()x"), -1);
  EXPECT_EQ(offsetOf("/body/DocFragment[1]/body/p[0]"), -1);
}

// One firmware-generated position, re-resolved by the firmware's own reader side.
struct Generated {
  int spine;
  const char* method;
  int paragraph;  // 1-based <p> for "paragraph", 0 for "progress"
  float fraction;
  std::string xpointer;
  size_t visibleChar;
  int64_t textOffset = -1;  // chapter text offset for "offset"
};

bool isSpace(const std::string& cp) {
  return cp == " " || cp == "\n" || cp == "\t" || cp == "\r" || cp == "\xC2\xA0" || cp == "\xE3\x80\x80";
}

// "offset": what the reader records for a laid-out page -- the XPointer of the character at
// a chapter text offset (ChapterXPathResolver::findXPathForTextOffset). Offsets at the body's
// start (usually a heading) and at fractions of its text; each must land on that character
// (or, inside a whitespace-only run, on the next one) and read back to the same offset.
void generateOffsets(const Book& book, const int spine, std::vector<Generated>& out,
                     std::vector<std::string>& failures) {
  const VisibleText& text = book.text[spine];
  if (text.bodyEnd <= text.bodyStart) return;
  const size_t bodyLength = text.bodyEnd - text.bodyStart;
  for (const float fraction : {0.0f, 0.2f, 0.5f, 0.8f}) {
    Generated g{spine, "offset", 0, fraction, {}, 0};
    g.textOffset = static_cast<int64_t>(fraction * static_cast<float>(bodyLength));
    g.xpointer = ChapterXPathResolver::findXPathForTextOffset(book.epub, spine, static_cast<uint32_t>(g.textOffset));
    std::ostringstream where;
    where << "spine " << spine << " offset " << g.textOffset << " '" << g.xpointer << "'";
    if (g.xpointer.empty()) {
      failures.push_back(where.str() + ": not generated");
      continue;
    }
    const XPathSpineTarget target = ProgressMapper::locateInSpine(book.epub, spine, g.xpointer);
    if (!target.found || target.visibleChar < text.bodyStart + static_cast<size_t>(g.textOffset)) {
      failures.push_back(where.str() + ": does not resolve at or after the offset");
      continue;
    }
    for (size_t i = text.bodyStart + static_cast<size_t>(g.textOffset); i < target.visibleChar; ++i) {
      if (!isSpace(text.chars[i])) {
        failures.push_back(where.str() + ": skipped text");
        break;
      }
    }
    uint32_t back = 0;
    if (!ChapterXPathResolver::findTextOffsetForXPath(book.epub, spine, g.xpointer, back) ||
        back != target.visibleChar - text.bodyStart) {
      failures.push_back(where.str() + ": does not read back");
    }
    g.visibleChar = target.visibleChar;
    out.push_back(g);
  }
}

// Reader -> app: the positions a closed book would report (docs/reading-progress-v1.md).
// "paragraph" is what the reader records (Section's paragraph LUT + fraction of that
// paragraph); "progress" is the fallback without a LUT (fraction of the item's
// paragraph text). Every one must round-trip through the firmware's resolver.
std::vector<Generated> generate(const Book& book, std::vector<std::string>& failures) {
  std::vector<Generated> out;
  for (int spine = 0; spine < book.epub->getSpineItemsCount(); ++spine) {
    const VisibleText& text = book.text[spine];
    std::vector<std::pair<int, float>> inputs;  // (paragraph, fraction); paragraph 0 = progress
    for (const float f : {0.3f, 0.7f}) inputs.emplace_back(0, f);
    const int paragraphs = static_cast<int>(text.paragraphs.size());
    if (paragraphs > 0) {
      inputs.emplace_back(1, 0.0f);
      int longest = 1;
      for (int i = 1; i <= paragraphs; ++i) {
        const auto& r = text.paragraphs[i - 1];
        const auto& best = text.paragraphs[longest - 1];
        if (r.second - r.first > best.second - best.first) longest = i;
      }
      for (const float f : {0.1f, 0.5f, 0.9f}) inputs.emplace_back(longest, f);
    }
    for (const auto& [paragraph, fraction] : inputs) {
      Generated g{spine, paragraph ? "paragraph" : "progress", paragraph, fraction, {}, 0};
      g.xpointer = paragraph ? ChapterXPathResolver::findXPathForParagraphProgress(
                                   book.epub, spine, static_cast<uint16_t>(paragraph), fraction)
                             : ChapterXPathResolver::findXPathForProgress(book.epub, spine, fraction);
      std::ostringstream where;
      where << "spine " << spine << " " << g.method << " p" << paragraph << " f" << fraction << " '" << g.xpointer
            << "'";
      if (g.xpointer.rfind("/body/DocFragment[" + std::to_string(spine + 1) + "]", 0) != 0) {
        if (!paragraph && text.paragraphs.empty()) continue;  // no <p>/<li> text to point into
        failures.push_back(where.str() + ": not generated");
        continue;
      }
      const XPathSpineTarget target = ProgressMapper::locateInSpine(book.epub, spine, g.xpointer);
      if (!target.found || target.visibleChar > text.size()) {
        failures.push_back(where.str() + ": does not resolve");
        continue;
      }
      g.visibleChar = target.visibleChar;
      if (paragraph) {
        // It must land inside the paragraph, at the fraction of its text (a character
        // counted in <p>/<li>; nested <li> text is part of the paragraph's range).
        const auto& range = text.paragraphs[paragraph - 1];
        const size_t expected =
            range.first + static_cast<size_t>(fraction * static_cast<float>(range.second - range.first));
        const bool inRange = g.visibleChar >= range.first && g.visibleChar < std::max(range.second, range.first + 1);
        const size_t slack = (range.second - range.first) / 50 + 2;  // whitespace between inline runs
        if (!inRange || (fraction > 0.0f && (g.visibleChar + slack < expected || g.visibleChar > expected + slack))) {
          std::ostringstream detail;
          detail << where.str() << ": landed at " << g.visibleChar << ", paragraph [" << range.first << ", "
                 << range.second << ") expected ~" << expected;
          failures.push_back(detail.str());
        }
      } else if (g.visibleChar < text.size() && !text.inParagraph[g.visibleChar]) {
        failures.push_back(where.str() + ": landed outside <p>/<li> text");
      }
      out.push_back(g);
    }
    generateOffsets(book, spine, out, failures);
  }
  return out;
}

void addBook(JsonArray books, const char* file, const Book& book, const std::vector<Generated>& positions) {
  JsonObject entry = books.add<JsonObject>();
  entry["epub"] = file;
  JsonArray list = entry["positions"].to<JsonArray>();
  for (const auto& g : positions) {
    JsonObject row = list.add<JsonObject>();
    row["sectionIndex"] = g.spine;
    row["method"] = g.method;
    if (g.paragraph) row["paragraph"] = g.paragraph;
    if (g.textOffset >= 0) {
      row["offset"] = g.textOffset;
    } else {
      row["fraction"] = g.fraction;
    }
    row["xpointer"] = g.xpointer;
    const VisibleText& text = book.text[g.spine];
    const size_t end = std::min(g.visibleChar + 12, text.bodyEnd);
    row["textAt"] = textFrom(text, g.visibleChar, end > g.visibleChar ? end - g.visibleChar : 0);
  }
}

TEST(ReadingProgressXPointer, FirmwareXPointersRoundTripAndMatchGolden) {
  JsonDocument doc;
  doc["generator"] = "pocket-daily-firmware test/reading_progress (ChapterXPathResolver)";
  doc["note"] =
      "textAt: up to 12 codepoints of the body's character data (DOM text, whitespace kept, head/style/"
      "script/title excluded) starting at the XPointer, stopping at </body>. "
      "Regenerate with READING_PROGRESS_UPDATE_GOLDEN=1.";
  JsonArray books = doc["books"].to<JsonArray>();
  for (const char* file : {"pocket-daily-epub-check.epub", "frankenstein-se.epub"}) {
    const Book book = openBook(file);
    ASSERT_TRUE(book.epub) << file;
    std::vector<std::string> failures;
    const auto positions = generate(book, failures);
    for (const auto& failure : failures) ADD_FAILURE() << file << ": " << failure;
    EXPECT_GT(positions.size(), 10u) << file;
    addBook(books, file, book, positions);
  }
  std::string json;
  serializeJsonPretty(doc, json);
  // ArduinoJson's pretty printer ends lines with CRLF; keep the fixture LF-only.
  for (size_t at = json.find("\r\n"); at != std::string::npos; at = json.find("\r\n", at)) json.erase(at, 1);
  json += "\n";
  std::ofstream(std::string(READING_PROGRESS_OUTPUT) + "/firmware-xpointers.json", std::ios::binary) << json;

  const std::string goldenPath = std::string(READING_PROGRESS_FIXTURES) + "/firmware-xpointers.json";
  if (std::getenv("READING_PROGRESS_UPDATE_GOLDEN")) {
    std::ofstream(goldenPath, std::ios::binary) << json;
  }
  EXPECT_EQ(readFile(goldenPath), json) << "firmware XPointers changed; the app fixture must be re-verified";
}

}  // namespace
