// The reader's layout-ahead cadence and page pipeline against the real EPUB parser,
// layout and renderer, with an aphorism-style book (test/epubs/short-spines.epub) where
// nearly every page turn enters a new chapter.
#include <Epub.h>
#include <Epub/Page.h>
#include <Epub/Section.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "activities/reader/ReaderLayoutAhead.h"
#include "activities/reader/ReaderPageRenderer.h"

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
