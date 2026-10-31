// The reader's layout-ahead cadence and page pipeline against the real EPUB parser,
// layout and renderer, with an aphorism-style book (test/epubs/short-spines.epub) where
// nearly every page turn enters a new chapter.
#include <Epub.h>
#include <Epub/Page.h>
#include <Epub/ParsedText.h>
#include <Epub/Section.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "activities/reader/ReaderLayoutAhead.h"
#include "activities/reader/ReaderPageRenderer.h"
#include "activities/reader/ReaderProgressCodec.h"
#include "pocket_daily/ReaderPerf.h"

// Largest single allocation while `trackAllocations` is set: the X3 only builds a
// section while its largest free heap block is at least 12 KB, so no build step may
// need a bigger contiguous block (-fno-exceptions turns a failed one into abort()).
std::atomic<bool> trackAllocations{false};
std::atomic<size_t> largestAllocation{0};

void* trackedAllocate(const std::size_t size) {
  if (trackAllocations.load(std::memory_order_relaxed)) {
    size_t seen = largestAllocation.load(std::memory_order_relaxed);
    while (size > seen && !largestAllocation.compare_exchange_weak(seen, size)) {
    }
  }
  if (void* p = std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}

void* operator new(const std::size_t size) { return trackedAllocate(size); }
void* operator new[](const std::size_t size) { return trackedAllocate(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
constexpr int FONT_ID = 1;

// ASCII 32..126 as 6x10 2-bit glyphs (black, both grays, white), advance 7 px.
struct TestFont {
  static constexpr int GLYPHS = 95;
  static constexpr int BYTES = 15;  // 6 * 10 * 2 bits
  uint8_t bitmap[GLYPHS * BYTES];
  EpdGlyph glyphs[GLYPHS]{};
  EpdUnicodeInterval interval{32, 126, 0};
  EpdFontData data{};
  EpdFont font{&data};
  TestFont() {
    for (int i = 0; i < GLYPHS * BYTES; ++i) bitmap[i] = static_cast<uint8_t>(0x1B + 37 * i);
    for (int i = 0; i < GLYPHS; ++i) {
      glyphs[i] = {6, 10, 112, 0, 10, BYTES, static_cast<uint32_t>(i * BYTES)};
    }
    glyphs[0] = {0, 0, 112, 0, 0, 0, 0};  // space
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

class ReaderLayout : public testing::Test {
 protected:
  void SetUp() override {
    char dir[] = "/tmp/reader-layout-XXXXXX";
    ASSERT_NE(mkdtemp(dir), nullptr);
    TestFs::root = dir;
    ASSERT_EQ(::mkdir((TestFs::root + "/.crosspoint").c_str(), 0755), 0);
    std::ifstream in(READER_LAYOUT_EPUB, std::ios::binary);
    std::ofstream out(TestFs::root + "/book.epub", std::ios::binary);
    out << in.rdbuf();
    out.close();
    renderer.begin();
    renderer.setOrientation(GfxRenderer::LandscapeClockwise);
    renderer.setFontCacheManager(&cache);
    renderer.insertFont(FONT_ID, EpdFontFamily(&font.font));
    epub = std::make_shared<Epub>("/book.epub", "/.crosspoint");
    ASSERT_TRUE(epub->load(true, false));
    layout.fontId = FONT_ID;
    layout.lineCompression = 1.0f;
    layout.extraParagraphSpacing = true;
    layout.viewportWidth = 300;  // small page: several chapters span two or three pages
    layout.viewportHeight = 160;
    layout.embeddedStyle = true;
  }
  void TearDown() override {
    const std::string cmd = "rm -rf '" + TestFs::root + "'";
    ASSERT_EQ(std::system(cmd.c_str()), 0);
  }

  TestFont font;
  HalDisplay panel;
  GfxRenderer renderer{panel};
  FontCacheManager cache{renderer.getFontMap(), renderer.getSdCardFonts()};
  std::shared_ptr<Epub> epub;
  SectionLayout layout;
};

// A reader turning forward through the whole book (EpubReaderActivity::pageTurn/render),
// with `budget` pages of layout fitting between two turns (-1: idle, no limit).
struct Reading {
  std::shared_ptr<Epub> epub;
  GfxRenderer& renderer;
  SectionLayout layout;
  ReaderLayoutAhead ahead;
  std::unique_ptr<Section> section;
  int spine = 0;
  // turnSteps: build steps (Section::buildSomeMore(1): one page, or the parser's final flush)
  // on the turn path; turnPages: pages they laid out.
  uint32_t turnPages = 0, turnSteps = 0, maxTurnSteps = 0, chapterTurns = 0, adoptedTurns = 0, bookPages = 0;
  uint32_t heap = 100000;

  Reading(std::shared_ptr<Epub> e, GfxRenderer& r, const SectionLayout& l)
      : epub(e), renderer(r), layout(l), ahead(e, r) {}

  // Render path: open (or adopt) the section and lay out up to the page shown, one page per step.
  void show(const int page) {
    uint32_t built = 0, steps = 0;
    if (!section) {
      section = ahead.adopt(spine, layout);
      if (section) {
        ++adoptedTurns;
      } else {
        section = std::make_unique<Section>(epub, spine, renderer);
        if (!(layout.load(*section) && !section->isPartial())) ASSERT_TRUE(layout.startBuild(*section));
      }
      section->currentPage = page;
    }
    while (section->isBuilding() && !section->isBuildComplete() &&
           section->currentPage >= static_cast<int>(section->pageCount)) {
      const uint16_t before = section->pageCount;
      ASSERT_TRUE(section->buildSomeMore(1));
      built += section->pageCount - before;
      ++steps;
    }
    turnPages += built;
    turnSteps += steps;
    maxTurnSteps = std::max(maxTurnSteps, steps);
  }

  // What the render task does after the page (AA done) until the next button: it yields
  // once `budget` pages were laid out (the next turn's button).
  struct Gap {
    Reading* reading;
    uint32_t start;
    int budget;
  };
  void idle(const int budget) {
    Gap gap{this, ahead.pagesLaidOut(), budget};
    const auto yield = [](void* c) {
      const auto* g = static_cast<const Gap*>(c);
      return g->budget >= 0 && static_cast<int>(g->reading->ahead.pagesLaidOut() - g->start) >= g->budget;
    };
    const auto largestBlock = [](void* c) { return static_cast<const Gap*>(c)->reading->heap; };
    ASSERT_NE(ahead.afterPage(section.get(), spine, layout, yield, largestBlock, &gap),
              ReaderLayoutAhead::Result::ShownFailed);
  }

  // Forward turn (EpubReaderActivity::pageTurn).
  void forward() {
    if (section->currentPage < static_cast<int>(section->pageCount) - 1 || section->mayHaveMorePages()) {
      section->currentPage++;
      show(section->currentPage);
      return;
    }
    bookPages += section->pageCount;
    ++spine;
    section.reset();
    ++chapterTurns;
    show(0);
  }

  void readWholeBook(const int budget) {
    show(0);
    while (true) {
      idle(budget);
      if (spine + 1 >= epub->getSpineItemsCount() && section->currentPage >= static_cast<int>(section->pageCount) - 1 &&
          !section->mayHaveMorePages()) {
        break;
      }
      forward();
    }
    bookPages += section->pageCount;
  }
  uint32_t totalLaidOut() const { return turnPages + ahead.pagesLaidOut(); }

  // Backward turn from a chapter's first page: lands on the previous chapter's last page,
  // which the render path must lay out to the end unless it was laid out behind.
  bool backIntoPreviousChapter() {
    section.reset();
    --spine;
    const uint32_t stepsBefore = turnSteps;
    show(UINT16_MAX);
    while (section->isBuilding() && !section->isBuildComplete()) {
      const uint16_t before = section->pageCount;
      if (!section->buildSomeMore(1)) return false;
      turnPages += section->pageCount - before;
      ++turnSteps;
    }
    section->currentPage = section->pageCount - 1;
    return turnSteps == stepsBefore;  // true: nothing laid out on the turn
  }
};
}  // namespace

// Turns every ~300-500 ms leave room for about two pages of layout: every turn into the
// next chapter adopts a section built ahead, and every page is laid out exactly once.
TEST_F(ReaderLayout, FastTurnsAdoptEachChapterAndLayOutEveryPageOnce) {
  Reading reading(epub, renderer, layout);
  reading.readWholeBook(2);
  EXPECT_GT(reading.bookPages, static_cast<uint32_t>(epub->getSpineItemsCount()));  // some chapters span pages
  EXPECT_EQ(reading.chapterTurns, static_cast<uint32_t>(epub->getSpineItemsCount() - 1));
  EXPECT_EQ(reading.adoptedTurns, reading.chapterTurns);
  EXPECT_EQ(reading.totalLaidOut(), reading.bookPages);
  EXPECT_EQ(reading.turnSteps, 1U);  // only the book's first page, on opening
}

// No time at all between turns: nothing runs ahead, the turn lays out only the page it
// shows, and still no page is laid out twice.
TEST_F(ReaderLayout, BackToBackTurnsLayOutOnlyTheShownPage) {
  Reading reading(epub, renderer, layout);
  reading.readWholeBook(0);
  EXPECT_EQ(reading.ahead.pagesLaidOut(), 0U);
  EXPECT_EQ(reading.adoptedTurns, 0U);
  EXPECT_EQ(reading.totalLaidOut(), reading.bookPages);
  EXPECT_LE(reading.maxTurnSteps, 1U);
}

// An idle pause finishes the shown chapter and, from its last page, the next one.
TEST_F(ReaderLayout, IdlePauseCompletesTheChapterAndTheNext) {
  Reading reading(epub, renderer, layout);
  reading.readWholeBook(-1);
  EXPECT_EQ(reading.adoptedTurns, reading.chapterTurns);
  EXPECT_EQ(reading.turnSteps, 1U);  // only the book's first page, on opening
  EXPECT_EQ(reading.totalLaidOut(), reading.bookPages);
}

// Without heap headroom nothing is pre-built; turns still lay out each page once.
TEST_F(ReaderLayout, LowHeapNeverStartsAPreBuild) {
  Reading reading(epub, renderer, layout);
  reading.heap = 10 * 1024;
  reading.readWholeBook(-1);
  EXPECT_EQ(reading.adoptedTurns, 0U);
  EXPECT_EQ(reading.totalLaidOut(), reading.bookPages);
}

// Anti-aliasing (ReaderPageRenderer) yields only to a queued turn: with none it always
// completes (a page the reader stays on gets its gray planes); a turn queued before the
// pass skips it without touching controller RAM; one queued mid-pass cuts it and re-syncs.
TEST_F(ReaderLayout, AntiAliasingCompletesUnlessATurnIsQueued) {
  Section section(epub, 0, renderer);
  ASSERT_TRUE(layout.startBuild(section));
  ASSERT_TRUE(section.buildSomeMore(0));
  auto page = section.loadPage(0);
  ASSERT_NE(page, nullptr);

  static int checks = 0;
  static int queueAfter = -1;
  ReaderPageRenderer::Options options;
  options.fontId = FONT_ID;
  options.marginTop = 10;
  options.marginLeft = 10;
  options.textAntiAliasing = true;
  options.nextTurnQueued = [](void*) { return queueAfter >= 0 && ++checks > queueAfter; };
  int pagesUntilFullRefresh = 5;
  const auto run = [&](const int after) {
    checks = 0;
    queueAfter = after;
    const PanelCounters before = panel.c;
    renderer.clearScreen();
    ReaderPageRenderer::render(renderer, *page, options, pagesUntilFullRefresh);
    PanelCounters d;
    d.gray = panel.c.gray - before.gray;
    d.strips = panel.c.strips - before.strips;
    d.cleanups = panel.c.cleanups - before.cleanups;
    d.fast = panel.c.fast - before.fast;
    return d;
  };

  const PanelCounters stays = run(-1);
  EXPECT_EQ(stays.fast, 1U);
  EXPECT_EQ(stays.gray, 1U);
  EXPECT_EQ(stays.strips, 14U);  // 7 strips of 76 rows per plane on the X3
  EXPECT_EQ(stays.cleanups, 1U);

  const PanelCounters skipped = run(0);
  EXPECT_EQ(skipped.fast, 1U);  // the BW page is always shown
  EXPECT_EQ(skipped.gray, 0U);
  EXPECT_EQ(skipped.strips, 0U);
  EXPECT_EQ(skipped.cleanups, 0U);

  const PanelCounters cut = run(4);  // queued after three strips
  EXPECT_EQ(cut.gray, 0U);
  EXPECT_EQ(cut.strips, 3U);
  EXPECT_EQ(cut.cleanups, 1U);
}

// Layout behind: idle on the first page of a chapter (one spanning several pages; on a
// one-page chapter the next chapter comes first), the previous chapter is laid out too, so
// the backward turn into it (to its last page) lays out nothing on the turn.
TEST_F(ReaderLayout, IdleOnAFirstPageLaysOutThePreviousChapter) {
  int chapter = -1;
  for (int s = 2; s < epub->getSpineItemsCount() - 1 && chapter < 0; ++s) {
    Section probe(epub, s, renderer);
    ASSERT_TRUE(layout.startBuild(probe));
    ASSERT_TRUE(probe.buildSomeMore(0));
    if (probe.pageCount >= 2) chapter = s;
  }
  ASSERT_GE(chapter, 2) << "fixture needs a multi-page chapter";
  // Probing built every chapter up to `chapter`: start from clean section caches.
  for (int s = 0; s < epub->getSpineItemsCount(); ++s) Section(epub, s, renderer).clearCache();

  Reading reading(epub, renderer, layout);
  reading.spine = chapter;
  reading.show(0);
  reading.idle(-1);  // idle on its first page
  EXPECT_TRUE(reading.backIntoPreviousChapter());
  EXPECT_EQ(reading.adoptedTurns, 1U);
  EXPECT_EQ(reading.spine, chapter - 1);
  EXPECT_FALSE(reading.section->isBuilding());
  EXPECT_EQ(reading.section->currentPage, static_cast<int>(reading.section->pageCount) - 1);

  // Without the idle moment the turn lays the previous chapter out itself.
  for (int s = 0; s < epub->getSpineItemsCount(); ++s) Section(epub, s, renderer).clearCache();
  Reading busy(epub, renderer, layout);
  busy.spine = chapter;
  busy.show(0);
  busy.idle(0);
  EXPECT_FALSE(busy.backIntoPreviousChapter());
  EXPECT_EQ(busy.adoptedTurns, 0U);
}

// A short chapter can emit the requested page with only one input buffer left.
// Retaining its parser/CSS/unfinished page through glyph prewarm wastes heap.
TEST_F(ReaderLayout, FinishesShortTailBeforePageRenderWithoutChangingPageBytes) {
  for (const int paragraphs : {16, 22, 28}) {
    const auto htmlDir = epub->getCachePath() + "/html";
    Storage.mkdir(htmlDir.c_str());
    std::string html = "<html><body>";
    for (int i = 0; i < paragraphs; ++i) {
      html += "<p id='p" + std::to_string(i) + "'>Chapter " + std::to_string(i) +
              " has distinct words: north east south west, amber blue green.</p>";
    }
    html += "</body></html>";
    ASSERT_GT(html.size(), 1024U);
    ASSERT_LE(html.size(), 3072U);
    {
      std::ofstream out(TestFs::root + htmlDir + "/0.html");
      out << html;
    }
    const auto pages = [](Section& section) {
      std::vector<std::string> bytes;
      for (unsigned i = 0; i < section.pageCount; ++i) {
        auto page = section.loadPage(i);
        if (!page) return std::vector<std::string>{};
        HalFile file;
        if (!Storage.openFileForWrite("TEST", "/page.bin", file) || !page->serialize(file))
          return std::vector<std::string>{};
        file.close();
        std::ifstream in(TestFs::map("/page.bin"), std::ios::binary);
        bytes.emplace_back(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
      }
      return bytes;
    };
    Section reference(epub, 0, renderer);
    ASSERT_TRUE(layout.startBuild(reference));
    ASSERT_TRUE(reference.buildSomeMore(0));
    const auto expected = pages(reference);
    ASSERT_GT(expected.size(), 1U);
    reference.clearCache();
    Section incremental(epub, 0, renderer);
    ASSERT_TRUE(layout.startBuild(incremental));
    ASSERT_TRUE(incremental.buildSomeMore(1));
    EXPECT_TRUE(incremental.isBuildComplete());
    EXPECT_FALSE(incremental.isBuilding());
    EXPECT_EQ(pages(incremental), expected);
  }
}

TEST_F(ReaderLayout, LargeChapterStillYieldsAndCanResumeFromPartial) {
  std::ifstream in(LONG_PARAGRAPH_EPUB, std::ios::binary);
  std::ofstream out(TestFs::root + "/long.epub", std::ios::binary);
  out << in.rdbuf();
  out.close();
  auto book = std::make_shared<Epub>("/long.epub", "/.crosspoint");
  ASSERT_TRUE(book->load(true, false));
  Section section(book, 2, renderer);
  ASSERT_TRUE(layout.startBuild(section));
  ASSERT_TRUE(section.buildSomeMore(1));
  EXPECT_TRUE(section.isBuilding());
  EXPECT_FALSE(section.isBuildComplete());
  const auto count = section.pageCount;
  ASSERT_GT(count, 0U);
  section.suspendBuild();
  EXPECT_TRUE(section.isPartial());
  EXPECT_EQ(section.pageCount, count);
  ASSERT_NE(section.loadPage(0), nullptr);
  ASSERT_TRUE(layout.startBuild(section));
  ASSERT_TRUE(section.buildSomeMore(0));
  EXPECT_TRUE(section.isBuildComplete());
  EXPECT_GT(section.pageCount, count);
}

TEST_F(ReaderLayout, InvalidShortTailFailsRatherThanCommittingIncompletePages) {
  const auto htmlDir = epub->getCachePath() + "/html";
  Storage.mkdir(htmlDir.c_str());
  std::string html = "<html><body>";
  for (int i = 0; i < 16; ++i) {
    html += "<p>Distinct paragraph " + std::to_string(i) + " north east south west amber blue green.</p>";
  }
  html += "</invalid></body></html>";
  ASSERT_GT(html.size(), 1024U);
  ASSERT_LT(html.size(), 2048U);
  {
    std::ofstream out(TestFs::root + htmlDir + "/0.html");
    out << html;
  }
  Section section(epub, 0, renderer);
  ASSERT_TRUE(layout.startBuild(section));
  EXPECT_FALSE(section.buildSomeMore(1));
  EXPECT_FALSE(section.isBuilding());
  EXPECT_FALSE(section.isBuildComplete());
  EXPECT_EQ(section.pageCount, 0U);
}

TEST_F(ReaderLayout, EveryUnmarkedPageRenderHasFreshMemoryEvidenceAndDuplicatesAreRejected) {
  namespace Perf = PocketDaily::ReaderPerf;
  Perf::open();
  struct ClosePerf {
    ~ClosePerf() { Perf::close(); }
  } closePerf;
  Section section(epub, 0, renderer);
  ASSERT_TRUE(layout.startBuild(section));
  ASSERT_TRUE(section.buildSomeMore(0));
  ASSERT_GT(section.pageCount, 0U);
  ReaderPageRenderer::Options options;
  options.fontId = FONT_ID;
  options.textAntiAliasing = true;
  options.refreshFrequency = 3;
  int refresh = 0;
  uint32_t cursor = 0;
  Perf::TurnRecord record;
  EXPECT_FALSE(Perf::copyNewestAfter(cursor, record));
  for (unsigned i = 0; i < 20; ++i) {
    auto page = section.loadPage(i % section.pageCount);
    ASSERT_NE(page, nullptr);
    if (i == 0) {
      Perf::beginTurn(0, Perf::FLAG_OPEN);
      EXPECT_FALSE(Perf::copyNewestAfter(cursor, record));
    }
    // Later automatic/native turns deliberately have no explicit beginTurn.
    ReaderPageRenderer::render(renderer, *page, options, refresh);
    Perf::endTurn();
    ASSERT_TRUE(Perf::copyNewestAfter(cursor, record)) << i;
    EXPECT_EQ((record.flags & Perf::FLAG_OPEN) != 0, i == 0);
    EXPECT_EQ((record.flags & Perf::FLAG_HALF) != 0, i % 3 == 0);
    EXPECT_GT(record.freeHeap, 0U);
    EXPECT_GT(record.largestBlock, 0U);
    const auto observed = cursor;
    EXPECT_FALSE(Perf::copyNewestAfter(cursor, record));
    EXPECT_EQ(cursor, observed);
  }
  Perf::TurnRecord oldest;
  EXPECT_TRUE(Perf::copyRecent(15, oldest));
  EXPECT_FALSE(Perf::copyRecent(16, oldest));
}

// A 30,000-character single paragraph (the reading-progress fixture's chapter 3) is laid
// out in bounded chunks. With 750-word chunks the word buffers grew to 1024 entries, a
// ~24 KB contiguous reserve that aborted an X3 build resumed with a 12 KB free block.
TEST_F(ReaderLayout, LongParagraphBuildNeedsNoLargeContiguousBlock) {
  std::ifstream in(LONG_PARAGRAPH_EPUB, std::ios::binary);
  std::ofstream out(TestFs::root + "/long.epub", std::ios::binary);
  out << in.rdbuf();
  out.close();
  auto book = std::make_shared<Epub>("/long.epub", "/.crosspoint");
  ASSERT_TRUE(book->load(true, false));
  ASSERT_GE(book->getSpineItemsCount(), 3);
  Section section(book, 2, renderer);
  ASSERT_TRUE(layout.startBuild(section));
  largestAllocation = 0;
  trackAllocations = true;
  const bool built = section.buildSomeMore(0);
  trackAllocations = false;
  ASSERT_TRUE(built);
  EXPECT_TRUE(section.isBuildComplete());
  EXPECT_GT(section.pageCount, 20U);
  EXPECT_LT(largestAllocation.load(), 12U * 1024U) << "a build step needs more than the X3's build threshold";
}

TEST(ParsedTextBuffers, WordCapacityStaysBoundedUpToTheChunkSize) {
  ParsedText text(false);
  for (size_t i = 0; i <= ParsedText::LONG_BLOCK_WORDS; ++i) text.addWord("word", EpdFontFamily::REGULAR);
  EXPECT_LE(text.wordCapacity(), ParsedText::WORD_CAPACITY_STEP_LIMIT);
  // A single very long word still fits; it only raises the capacity to what it needs.
  text.addWord(std::string(200, 'x'), EpdFontFamily::REGULAR);
  EXPECT_GE(text.wordCapacity(), text.size());
  EXPECT_LE(text.wordCapacity(), ParsedText::LONG_BLOCK_WORDS + 1 + 200);
}

// A suspended build persists a partial section. The partial sentinel is the same for every
// section format, so the trailer carries the format it was written in: a partial from older
// firmware (8-byte trailer, v132 page records without the text offset) must be rebuilt, not
// read as the current format (X3: a book reopened at its chapter start after an update).
TEST_F(ReaderLayout, PartialFromAnotherFormatIsRebuilt) {
  std::ifstream in(LONG_PARAGRAPH_EPUB, std::ios::binary);
  std::ofstream out(TestFs::root + "/long.epub", std::ios::binary);
  out << in.rdbuf();
  out.close();
  auto book = std::make_shared<Epub>("/long.epub", "/.crosspoint");
  ASSERT_TRUE(book->load(true, false));
  const std::string path = TestFs::root + book->getCachePath() + "/sections/2.bin";
  {
    Section section(book, 2, renderer);
    ASSERT_TRUE(layout.startBuild(section));
    ASSERT_TRUE(section.buildSomeMore(3));
    section.suspendBuild();
  }
  {
    Section section(book, 2, renderer);
    ASSERT_TRUE(layout.load(section));
    EXPECT_TRUE(section.isPartial());
  }
  std::ifstream partialIn(path, std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(partialIn)), {});
  partialIn.close();
  ASSERT_GT(bytes.size(), 12U);

  // Older firmware: the same trailer without the format tag.
  {
    std::ofstream old(path, std::ios::binary | std::ios::trunc);
    old.write(bytes.data(), static_cast<std::streamsize>(bytes.size() - 4));
  }
  {
    Section section(book, 2, renderer);
    EXPECT_FALSE(layout.load(section));
    EXPECT_FALSE(section.isPartial());
  }
  EXPECT_FALSE(std::ifstream(path).good()) << "the stale partial is cleared";

  // A tag naming another section format.
  {
    std::string other = bytes;
    other[other.size() - 4] = static_cast<char>(other[other.size() - 4] ^ 0x01);
    std::ofstream write(path, std::ios::binary | std::ios::trunc);
    write.write(other.data(), static_cast<std::streamsize>(other.size()));
  }
  Section section(book, 2, renderer);
  EXPECT_FALSE(layout.load(section));
}

TEST_F(ReaderLayout, UpgradedSectionRejectsOldVersionAndChangedSpacing) {
  const std::string path = TestFs::root + epub->getCachePath() + "/sections/0.bin";
  {
    Section section(epub, 0, renderer);
    ASSERT_TRUE(layout.startBuild(section));
    ASSERT_TRUE(section.buildSomeMore(0));
    ASSERT_TRUE(section.isBuildComplete());
  }
  {
    std::fstream cache(path, std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(cache.good());
    EXPECT_EQ(cache.get(), 137);
    cache.seekp(0);
    cache.put(static_cast<char>(135));
  }
  {
    Section stale(epub, 0, renderer);
    EXPECT_FALSE(layout.load(stale));
  }
  for (bool wordSpacing : {false, true}) {
    Section built(epub, 0, renderer);
    ASSERT_TRUE(layout.startBuild(built));
    ASSERT_TRUE(built.buildSomeMore(0));
    SectionLayout changed = layout;
    if (wordSpacing)
      changed.wordSpacingPercent = 120;
    else
      changed.characterSpacing = 1;
    Section reopened(epub, 0, renderer);
    EXPECT_FALSE(changed.load(reopened));
  }
}

TEST_F(ReaderLayout, Version135RebuildPreservesProgressMetadataAndOffers) {
  std::ifstream fixture(LONG_PARAGRAPH_EPUB, std::ios::binary);
  std::ofstream copied(TestFs::root + "/long.epub", std::ios::binary);
  copied << fixture.rdbuf();
  copied.close();
  auto book = std::make_shared<Epub>("/long.epub", "/.crosspoint");
  ASSERT_TRUE(book->load(true, false));
  const std::string cache = TestFs::root + book->getCachePath();
  const std::string sectionPath = cache + "/sections/2.bin";
  auto contents = [](const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), {});
  };
  const ReaderProgressCodec::Position saved{2, 1, 42, 33, 1234};
  uint8_t progress[ReaderProgressCodec::EXTENDED_SIZE];
  ASSERT_EQ(ReaderProgressCodec::encode(saved, progress), sizeof(progress));
  {
    std::ofstream file(cache + "/progress.bin", std::ios::binary);
    file.write(reinterpret_cast<const char*>(progress), sizeof(progress));
  }
  // Opaque existing records are outside section invalidation's ownership.
  std::ofstream(cache + "/pocket-reading.bin", std::ios::binary) << "existing reading record";
  std::ofstream(cache + "/pocket-reading-offer.bin", std::ios::binary) << "existing pending offer";
  const auto metadata = contents(cache + "/book.bin");
  ASSERT_FALSE(metadata.empty());
  for (bool partial : {false, true}) {
    {
      Section built(book, 2, renderer);
      ASSERT_TRUE(layout.startBuild(built));
      ASSERT_TRUE(built.buildSomeMore(partial ? 3 : 0));
      if (partial) built.suspendBuild();
    }
    const auto html = contents(cache + "/html/2.html");
    ASSERT_FALSE(html.empty());
    auto old = contents(sectionPath);
    ASSERT_GT(old.size(), 12U);
    if (partial) {
      ASSERT_EQ(static_cast<uint8_t>(old[0]), 0xFF);
      ASSERT_EQ(static_cast<uint8_t>(old[old.size() - 4]), 137);
      old[old.size() - 4] = static_cast<char>(135);
    } else {
      ASSERT_EQ(static_cast<uint8_t>(old[0]), 137);
      old[0] = static_cast<char>(135);
    }
    {
      std::ofstream file(sectionPath, std::ios::binary | std::ios::trunc);
      file.write(old.data(), static_cast<std::streamsize>(old.size()));
    }
    Section stale(book, 2, renderer);
    EXPECT_FALSE(layout.load(stale));
    EXPECT_FALSE(std::ifstream(sectionPath).good());
    Section rebuilt(book, 2, renderer);
    ASSERT_TRUE(layout.startBuild(rebuilt));
    ASSERT_TRUE(rebuilt.buildSomeMore(0));
    Section reopened(book, 2, renderer);
    ASSERT_TRUE(layout.load(reopened));
    ASSERT_NE(reopened.loadPage(saved.page), nullptr);
    EXPECT_EQ(contents(cache + "/book.bin"), metadata);
    EXPECT_EQ(contents(cache + "/html/2.html"), html);
    EXPECT_EQ(contents(cache + "/progress.bin"),
              std::string(reinterpret_cast<const char*>(progress), sizeof(progress)));
    EXPECT_EQ(contents(cache + "/pocket-reading.bin"), "existing reading record");
    EXPECT_EQ(contents(cache + "/pocket-reading-offer.bin"), "existing pending offer");
  }
}
