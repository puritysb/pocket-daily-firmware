// Exact reading positions against the real layout (docs/reading-progress-v1.md): every
// laid-out page carries the chapter text offset of its first character, the XPointer the
// reader records for a page points at exactly that character, and an XPointer maps back
// to the page holding its character.
//
// The expected text comes from an independent scan of the chapter's XHTML, and XPointers
// are resolved by ProgressMapper::locateInSpine, the resolver checked against the app's own
// positions in test/reading_progress.
#include <ArduinoJson.h>
#include <ChapterXPathResolver.h>
#include <Epub.h>
#include <Epub/Page.h>
#include <Epub/Section.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <ProgressMapper.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Epub/htmlEntities.h"
#include "activities/reader/ReaderLayoutAhead.h"

namespace {
constexpr int FONT_ID = 1;

// ASCII 32..126 as 6x10 2-bit glyphs, advance 7 px (other code points use the missing-glyph mark).
struct TestFont {
  static constexpr int GLYPHS = 95;
  static constexpr int BYTES = 15;
  uint8_t bitmap[GLYPHS * BYTES];
  EpdGlyph glyphs[GLYPHS]{};
  EpdUnicodeInterval interval{32, 126, 0};
  EpdFontData data{};
  EpdFont font{&data};
  TestFont() {
    for (int i = 0; i < GLYPHS * BYTES; ++i) bitmap[i] = static_cast<uint8_t>(0x1B + 37 * i);
    for (int i = 0; i < GLYPHS; ++i) glyphs[i] = {6, 10, 112, 0, 10, BYTES, static_cast<uint32_t>(i * BYTES)};
    glyphs[0] = {0, 0, 112, 0, 0, 0, 0};
    data.bitmap = bitmap;
    data.glyph = glyphs;
    data.intervals = &interval;
    data.intervalCount = 1;
    data.advanceY = 14;
    data.ascender = 10;
    data.descender = -3;
    data.is2Bit = true;
  }
};

class StringSink final : public Print {
 public:
  std::string text;
  size_t write(uint8_t c) override {
    text.push_back(static_cast<char>(c));
    return 1;
  }
  size_t write(const uint8_t* buffer, size_t size) override {
    text.append(reinterpret_cast<const char*>(buffer), size);
    return size;
  }
};

size_t utf8Length(const unsigned char lead) {
  if (lead < 0x80) return 1;
  if ((lead & 0xE0) == 0xC0) return 2;
  if ((lead & 0xF0) == 0xE0) return 3;
  if ((lead & 0xF8) == 0xF0) return 4;
  return 1;
}

void appendCodepoint(const uint32_t cp, std::string& out) {
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

// Independent reference: the character data of an XHTML chapter as a DOM holds it (entities
// decoded, CRLF/CR as LF), one string per code point. `bodyStart` is where <body>'s text
// begins; `visibleBeforeBody` counts what ProgressMapper's streamer sees before it (text
// outside head/style/script/title), so its visibleChar maps to a body offset.
struct ChapterText {
  std::vector<std::string> body;
  size_t visibleBeforeBody = 0;
};

ChapterText chapterText(const std::string& xhtml) {
  ChapterText out;
  bool inBody = false;
  int hidden = 0;
  bool lastCr = false;
  const auto push = [&](std::string cp) {
    if (inBody) {
      out.body.push_back(std::move(cp));
    } else if (hidden == 0) {
      out.visibleBeforeBody++;
    }
  };
  for (size_t i = 0; i < xhtml.size();) {
    const char c = xhtml[i];
    if (c == '<') {
      if (xhtml.compare(i, 4, "<!--") == 0) {
        const size_t end = xhtml.find("-->", i);
        i = end == std::string::npos ? xhtml.size() : end + 3;
        continue;
      }
      const size_t end = xhtml.find('>', i);
      if (end == std::string::npos) break;
      const std::string tag = xhtml.substr(i + 1, end - i - 1);
      i = end + 1;
      lastCr = false;
      if (tag.empty() || tag[0] == '!' || tag[0] == '?') continue;
      const bool close = tag[0] == '/';
      const bool selfClose = tag.back() == '/';
      const size_t nameStart = close ? 1 : 0;
      const size_t nameEnd = tag.find_first_of(" \t\r\n/", nameStart);
      std::string name = tag.substr(nameStart, nameEnd == std::string::npos ? std::string::npos : nameEnd - nameStart);
      for (auto& ch : name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      if (!inBody && !close && name == "body") inBody = true;
      if (inBody && close && name == "body") break;
      const bool nonVisible = name == "head" || name == "style" || name == "script" || name == "title";
      if (!inBody && nonVisible && !selfClose) hidden += close ? -1 : 1;
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
          for (size_t k = 0; k < decoded.size();) {
            const size_t n = utf8Length(static_cast<unsigned char>(decoded[k]));
            push(decoded.substr(k, n));
            k += n;
          }
          i = semi + 1;
          continue;
        }
      }
    }
    const size_t n = std::min(utf8Length(static_cast<unsigned char>(c)), xhtml.size() - i);
    push(xhtml.substr(i, n));
    i += n;
  }
  return out;
}

bool isLayoutSpace(const std::string& cp) {
  return cp == " " || cp == "\n" || cp == "\t" || cp == "\r" || cp == "\xEF\xBB\xBF";
}

// True when the body text at `offset` reads `word` as the layout shows it: soft hyphens are
// not drawn, a no-break space is drawn as a space, and a hyphenated prefix ends in '-'.
bool bodyStartsWithWord(const ChapterText& text, size_t offset, std::string word) {
  if (!word.empty() && word.back() == '-') word.pop_back();  // a hyphenation split adds it
  size_t at = 0;
  while (at < word.size()) {
    if (offset >= text.body.size()) return false;
    const std::string& cp = text.body[offset++];
    if (cp == "\xC2\xAD") continue;  // soft hyphen
    const std::string shown = (cp == "\xC2\xA0" || cp == "\xE2\x80\xAF") ? std::string(" ") : cp;
    if (word.compare(at, shown.size(), shown) != 0) return false;
    at += shown.size();
  }
  return true;
}

std::string bodyTextAt(const ChapterText& text, size_t offset, size_t count) {
  std::string out;
  for (size_t i = offset; i < text.body.size() && i < offset + count; ++i) out += text.body[i];
  return out;
}

class ReadingPositionLayout : public testing::Test {
 protected:
  void SetUp() override {
    char dir[] = "/tmp/reading-position-XXXXXX";
    ASSERT_NE(mkdtemp(dir), nullptr);
    TestFs::root = dir;
    ASSERT_EQ(::mkdir((TestFs::root + "/.crosspoint").c_str(), 0755), 0);
    renderer.begin();
    renderer.setOrientation(GfxRenderer::Portrait);
    renderer.setFontCacheManager(&cache);
    renderer.insertFont(FONT_ID, EpdFontFamily(&font.font));
    layout.fontId = FONT_ID;
    layout.lineCompression = 1.0f;
    layout.extraParagraphSpacing = true;
    layout.viewportWidth = 300;
    layout.viewportHeight = 400;
    layout.embeddedStyle = true;
  }
  void TearDown() override {
    const std::string cmd = "rm -rf '" + TestFs::root + "'";
    ASSERT_EQ(std::system(cmd.c_str()), 0);
  }

  std::shared_ptr<Epub> open(const char* fixture) {
    std::ifstream in(std::string(READING_PROGRESS_FIXTURES) + "/" + fixture, std::ios::binary);
    std::ofstream out(TestFs::root + "/" + fixture, std::ios::binary);
    out << in.rdbuf();
    out.close();
    auto book = std::make_shared<Epub>(std::string("/") + fixture, "/.crosspoint");
    return book->load(true, false) ? book : nullptr;
  }

  static ChapterText textOf(const std::shared_ptr<Epub>& book, const int spine) {
    StringSink sink;
    book->readSpineItemToStream(spine, sink, 1024);
    return chapterText(sink.text);
  }

  std::unique_ptr<Section> build(const std::shared_ptr<Epub>& book, const int spine) {
    auto section = std::make_unique<Section>(book, spine, renderer);
    if (!layout.startBuild(*section)) return nullptr;
    while (!section->isBuildComplete()) {
      if (!section->buildSomeMore(0)) return nullptr;
    }
    return section;
  }

  // For every page after the first: the stamped offset is its first character, the recorded
  // XPointer lands exactly there (and back), and the page lookup returns the page for its
  // first and last character. Returns the pages checked.
  int checkChapter(const std::shared_ptr<Epub>& book, const int spine) {
    const ChapterText text = textOf(book, spine);
    auto section = build(book, spine);
    EXPECT_TRUE(section) << "spine " << spine;
    if (!section) return 0;
    int checked = 0;
    uint32_t previous = 0;
    for (int page = 0; page < section->pageCount; ++page) {
      const auto offset = section->getPageTextOffset(page);
      EXPECT_TRUE(offset.has_value()) << "spine " << spine << " page " << page;
      if (!offset) continue;
      EXPECT_GE(*offset, previous) << "page offsets must not decrease";
      previous = *offset;
      uint16_t found = 0;
      EXPECT_EQ(section->findPageForTextOffset(*offset, found), Section::TextOffsetLookup::Found);
      EXPECT_EQ(found, page) << "spine " << spine << " page " << page << " start";
      if (page == 0) continue;
      // The character before a page's first one belongs to the previous page (unless an
      // empty page repeats the offset).
      const auto before = section->getPageTextOffset(page - 1);
      if (before && *before < *offset) {
        EXPECT_EQ(section->findPageForTextOffset(*offset - 1, found), Section::TextOffsetLookup::Found);
        EXPECT_EQ(found, page - 1) << "spine " << spine << " page " << page << " start - 1";
      }

      const auto loaded = section->loadPage(page);
      EXPECT_TRUE(loaded);
      if (!loaded) continue;
      EXPECT_EQ(loaded->textOffset, *offset);
      if (!loaded->elements.empty() && loaded->elements.front()->getTag() == TAG_PageLine) {
        const auto& line = static_cast<const PageLine&>(*loaded->elements.front());
        const auto& words = *line.getBlock();
        if (words.wordCount() != 0) {
          EXPECT_TRUE(bodyStartsWithWord(text, *offset, words.wordText(0)))
              << "spine " << spine << " page " << page << ": first word '" << words.wordText(0) << "' but text at "
              << *offset << " is '" << bodyTextAt(text, *offset, 12) << "'";
        }
      }

      const std::string xpointer = ChapterXPathResolver::findXPathForTextOffset(book, spine, *offset);
      EXPECT_FALSE(xpointer.empty()) << "spine " << spine << " page " << page;
      if (xpointer.empty()) continue;
      const XPathSpineTarget target = ProgressMapper::locateInSpine(book, spine, xpointer);
      EXPECT_TRUE(target.found) << xpointer;
      EXPECT_EQ(target.visibleChar, text.visibleBeforeBody + *offset)
          << xpointer << " lands on '" << bodyTextAt(text, target.visibleChar - text.visibleBeforeBody, 12)
          << "', page starts '" << bodyTextAt(text, *offset, 12) << "'";
      uint32_t back = 0;
      EXPECT_TRUE(ChapterXPathResolver::findTextOffsetForXPath(book, spine, xpointer, back)) << xpointer;
      EXPECT_EQ(back, *offset) << xpointer;
      ++checked;
    }
    // The last character of each page maps to it.
    for (int page = 0; page + 1 < section->pageCount; ++page) {
      const auto next = section->getPageTextOffset(page + 1);
      const auto start = section->getPageTextOffset(page);
      if (!next || !start || *next == 0 || *next - 1 < *start) continue;
      uint16_t found = 0;
      EXPECT_EQ(section->findPageForTextOffset(*next - 1, found), Section::TextOffsetLookup::Found);
      EXPECT_EQ(found, page) << "spine " << spine << " page " << page << " end";
    }
    return checked;
  }

  TestFont font;
  HalDisplay panel;
  GfxRenderer renderer{panel};
  FontCacheManager cache{renderer.getFontMap(), renderer.getSdCardFonts()};
  SectionLayout layout;
};

// The X3 case: EPUB/section-3.xhtml is one 29,972-code-point paragraph over dozens of pages.
// The paragraph-fraction estimate the reader used to record lands pages away; the stamped
// offset is the page's first character.
TEST_F(ReadingPositionLayout, LongParagraphPagesRecordTheirFirstCharacter) {
  const auto book = open("pocket-daily-epub-check.epub");
  ASSERT_TRUE(book);
  constexpr int LONG_SPINE = 2;  // DocFragment[3]
  ASSERT_EQ(book->getSpineItem(LONG_SPINE).href.substr(book->getSpineItem(LONG_SPINE).href.rfind('/') + 1),
            "section-3.xhtml");
  EXPECT_GT(checkChapter(book, LONG_SPINE), 20);

  // What the old estimate recorded for page 10, for comparison with the exact position.
  auto section = build(book, LONG_SPINE);
  ASSERT_TRUE(section);
  uint16_t paragraph = 0, first = 0, last = 0;
  ASSERT_TRUE(section->getParagraphRunForPage(9, paragraph, first, last));
  const float fraction = (static_cast<float>(10 - first) - 0.5f) / static_cast<float>(last - first + 1);
  const std::string estimated =
      ChapterXPathResolver::findXPathForParagraphProgress(book, LONG_SPINE, paragraph, std::max(0.0f, fraction));
  const auto offset = section->getPageTextOffset(10);
  ASSERT_TRUE(offset);
  const std::string exact = ChapterXPathResolver::findXPathForTextOffset(book, LONG_SPINE, *offset);
  const ChapterText text = textOf(book, LONG_SPINE);
  const XPathSpineTarget old = ProgressMapper::locateInSpine(book, LONG_SPINE, estimated);
  std::printf("page 10/%u: exact %s ('%s'), estimate %s ('%s')\n", section->pageCount, exact.c_str(),
              bodyTextAt(text, *offset, 12).c_str(), estimated.c_str(),
              bodyTextAt(text, old.visibleChar - text.visibleBeforeBody, 12).c_str());
  EXPECT_NE(exact, estimated);
}

TEST_F(ReadingPositionLayout, EveryPageOfTheSampleBookRecordsItsFirstCharacter) {
  const auto book = open("pocket-daily-epub-check.epub");
  ASSERT_TRUE(book);
  int checked = 0;
  for (int spine = 0; spine < book->getSpineItemsCount(); ++spine) checked += checkChapter(book, spine);
  EXPECT_GT(checked, 50);
}

// A normal book: many paragraphs, headings, inline markup, entities, soft hyphens; laid out
// plain, with hyphenation (split words) and with focus reading (split tokens).
TEST_F(ReadingPositionLayout, EveryPageOfFrankensteinRecordsItsFirstCharacter) {
  const auto book = open("frankenstein-se.epub");
  ASSERT_TRUE(book);
  int checked = 0;
  for (int spine = 0; spine < book->getSpineItemsCount(); ++spine) checked += checkChapter(book, spine);
  EXPECT_GT(checked, 300);
}

TEST_F(ReadingPositionLayout, HyphenatedAndFocusLayoutsRecordTheirFirstCharacter) {
  const auto book = open("frankenstein-se.epub");
  ASSERT_TRUE(book);
  layout.hyphenationEnabled = true;
  layout.viewportWidth = 180;  // narrow: many words split at line ends
  int checked = 0;
  for (int spine = 0; spine < std::min(book->getSpineItemsCount(), 10); ++spine) checked += checkChapter(book, spine);
  layout.hyphenationEnabled = false;
  layout.focusReadingEnabled = true;
  for (int spine = 0; spine < std::min(book->getSpineItemsCount(), 10); ++spine) checked += checkChapter(book, spine);
  EXPECT_GT(checked, 100);
}

// App -> reader: an XPointer the app produced maps to the page holding its character.
TEST_F(ReadingPositionLayout, AppXPointersOpenThePageHoldingTheirCharacter) {
  for (const auto& [epubFile, fixtureFile] : {std::pair{"frankenstein-se.epub", "app-xpointers-frankenstein.json"},
                                              std::pair{"pocket-daily-epub-check.epub", "app-xpointers-korean.json"}}) {
    const auto book = open(epubFile);
    ASSERT_TRUE(book);
    std::ifstream in(std::string(READING_PROGRESS_FIXTURES) + "/" + fixtureFile, std::ios::binary);
    const std::string json{std::istreambuf_iterator<char>(in), {}};
    JsonDocument fixture;
    ASSERT_FALSE(deserializeJson(fixture, json));
    int mapped = 0;
    for (JsonObject position : fixture["positions"].as<JsonArray>()) {
      const std::string xpointer = position["xpointer"].as<std::string>();
      const std::string textAt = position["textAt"].as<std::string>();
      const int spine = position["sectionIndex"].as<int>();
      uint32_t offset = 0;
      ASSERT_TRUE(ChapterXPathResolver::findTextOffsetForXPath(book, spine, xpointer, offset)) << xpointer;
      const ChapterText text = textOf(book, spine);
      EXPECT_EQ(bodyTextAt(text, offset, textAt.empty() ? 0 : 1), textAt.substr(0, utf8Length(textAt[0]))) << xpointer;
      // toCrossPoint carries the same offset.
      const CrossPointPosition target = ProgressMapper::toCrossPoint(book, {xpointer, 0.5f}, renderer, spine, 10);
      EXPECT_TRUE(target.hasTextOffset);
      EXPECT_EQ(target.textOffset, offset) << xpointer;

      auto section = build(book, spine);
      ASSERT_TRUE(section);
      uint16_t page = 0;
      ASSERT_EQ(section->findPageForTextOffset(offset, page), Section::TextOffsetLookup::Found) << xpointer;
      const auto start = section->getPageTextOffset(page);
      ASSERT_TRUE(start);
      EXPECT_LE(*start, offset) << xpointer;
      if (page + 1 < section->pageCount) {
        const auto next = section->getPageTextOffset(page + 1);
        ASSERT_TRUE(next);
        EXPECT_GT(*next, offset) << xpointer;
      }
      ++mapped;
    }
    EXPECT_GT(mapped, 10) << fixtureFile;
  }
}

// Layout golden: every section file of two whole books, laid out plain, hyphenated narrow and
// with focus reading, hashed byte for byte. A change that alters layout must bump the section
// cache version and record the new digest here; a performance change must leave it alone
// (docs/fork-delta-register.md U-EPUB-1). The elapsed build time is logged, not asserted.
uint64_t fnv1a(const std::string& bytes, uint64_t hash) {
  for (const unsigned char c : bytes) hash = (hash ^ c) * 1099511628211ULL;
  return hash;
}

TEST_F(ReadingPositionLayout, SectionFilesMatchTheLayoutGolden) {
  struct Pass {
    bool hyphenation;
    bool focus;
    int width;
  };
  static constexpr Pass passes[] = {{false, false, 300}, {true, false, 180}, {false, true, 300}};
  uint64_t digest = 14695981039346656037ULL;
  size_t sectionBytes = 0;
  double buildMs = 0;
  for (const char* fixture : {"frankenstein-se.epub", "pocket-daily-epub-check.epub"}) {
    const auto book = open(fixture);
    ASSERT_TRUE(book) << fixture;
    for (const Pass& pass : passes) {
      layout.hyphenationEnabled = pass.hyphenation;
      layout.focusReadingEnabled = pass.focus;
      layout.viewportWidth = pass.width;
      for (int spine = 0; spine < book->getSpineItemsCount(); ++spine) {
        const auto start = std::chrono::steady_clock::now();
        auto section = build(book, spine);
        buildMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        ASSERT_TRUE(section) << fixture << " spine " << spine;
        section.reset();
        const std::string path = TestFs::root + book->getCachePath() + "/sections/" + std::to_string(spine) + ".bin";
        std::ifstream in(path, std::ios::binary);
        ASSERT_TRUE(in) << path;
        const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        sectionBytes += bytes.size();
        digest = fnv1a(bytes, digest);
        ASSERT_EQ(std::remove(path.c_str()), 0);
      }
    }
  }
  std::printf("[layout golden] digest=0x%016llx section_bytes=%zu build_ms=%.1f\n",
              static_cast<unsigned long long>(digest), sectionBytes, buildMs);
  EXPECT_EQ(digest, 0x6b8020d1993c5848ULL);
}

// While a section is still building, a character past the pages laid out so far needs more
// layout; one inside them is found at once.
TEST_F(ReadingPositionLayout, BuildingSectionAsksForMorePagesPastItsWatermark) {
  const auto book = open("pocket-daily-epub-check.epub");
  ASSERT_TRUE(book);
  Section section(book, 2, renderer);
  ASSERT_TRUE(layout.startBuild(section));
  ASSERT_TRUE(section.buildSomeMore(3));
  ASSERT_TRUE(section.isBuilding());
  ASSERT_GE(section.pageCount, 3);
  const auto second = section.getPageTextOffset(1);
  ASSERT_TRUE(second);
  uint16_t page = 0;
  EXPECT_EQ(section.findPageForTextOffset(*second, page), Section::TextOffsetLookup::Found);
  EXPECT_EQ(page, 1);
  EXPECT_EQ(section.findPageForTextOffset(20000, page), Section::TextOffsetLookup::NeedMorePages);
  while (!section.isBuildComplete()) ASSERT_TRUE(section.buildSomeMore(0));
  EXPECT_EQ(section.findPageForTextOffset(20000, page), Section::TextOffsetLookup::Found);
  EXPECT_GT(page, 3);
}

}  // namespace
