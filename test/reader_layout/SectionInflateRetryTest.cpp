// Real Section -> ZipFile -> miniz with bounded allocation fault injection.
#include <Epub.h>
#include <Epub/Page.h>
#include <Epub/Section.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <ZipFile.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

#include "ZipInflateFaults.h"
#include "activities/reader/ReaderLayoutAhead.h"

namespace {
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

std::string bytes(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
std::string removeHtmlAfterSecondReject;
void failThirdOpen(unsigned count) {
  if (count == 2) std::filesystem::remove_all(removeHtmlAfterSecondReject);
}

class SectionInflateRetry : public testing::Test {
 protected:
  void SetUp() override {
    ZipInflateFaults::disable();
    char dir[] = "/tmp/section-inflate-retry-XXXXXX";
    ASSERT_NE(mkdtemp(dir), nullptr);
    TestFs::root = dir;
    std::filesystem::create_directory(TestFs::root + "/.crosspoint");
    // This existing fixture has a 64 KiB third spine, so the tests still
    // exercise the streaming ring rather than the small-entry optimization.
    std::filesystem::copy_file(LONG_PARAGRAPH_EPUB, TestFs::root + "/book.epub");
    renderer.begin();
    renderer.setFontCacheManager(&cache);
    renderer.insertFont(1, EpdFontFamily(&font.font));
    epub = std::make_shared<Epub>("/book.epub", "/.crosspoint");
    ASSERT_TRUE(epub->load(true, false));
    layout.fontId = 1;
    layout.lineCompression = 1;
    layout.extraParagraphSpacing = true;
    layout.viewportWidth = 300;
    layout.viewportHeight = 160;
    layout.embeddedStyle = true;
  }
  void TearDown() override {
    EXPECT_EQ(ZipInflateFaults::stats().live, 0U);
    EXPECT_EQ(ZipInflateFaults::stats().canaryFailures, 0U);
    ZipInflateFaults::disable();
    epub.reset();
    std::filesystem::remove_all(TestFs::root);
  }
  std::string html() const { return TestFs::root + epub->getCachePath() + "/html/2.html"; }
  std::string partialHtml() const { return TestFs::root + epub->getCachePath() + "/html/.tmp_2.html"; }
  std::string sectionFile() const { return TestFs::root + epub->getCachePath() + "/sections/2.bin"; }
  void expectNoPromotedHtml() {
    EXPECT_FALSE(std::filesystem::exists(html()));
    EXPECT_FALSE(std::filesystem::exists(partialHtml()));
    EXPECT_FALSE(std::filesystem::exists(sectionFile()));
  }
  TestFont font;
  HalDisplay panel;
  GfxRenderer renderer{panel};
  FontCacheManager cache{renderer.getFontMap(), renderer.getSdCardFonts()};
  std::shared_ptr<Epub> epub;
  SectionLayout layout;
};

TEST_F(SectionInflateRetry, NormalInflateKeepsFastChunks) {
  Section section(epub, 2, renderer);
  ZipInflateFaults::reset();
  ASSERT_TRUE(layout.startBuild(section));
  EXPECT_EQ(ZipInflateFaults::stats().largeChunks, 2U);
  EXPECT_EQ(ZipInflateFaults::stats().smallChunks, 0U);
  EXPECT_EQ(ZipInflateFaults::stats().windowRejects, 0U);
  EXPECT_EQ(ZipInflateFaults::stats().live, 0U);
  ZipInflateFaults::disable();
  ASSERT_TRUE(section.buildSomeMore(0));
  EXPECT_GT(section.pageCount, 0U);
}

TEST_F(SectionInflateRetry, WindowRetryPreservesExactHtmlAndSectionBytes) {
  std::string baselineHtml, baselineSection;
  uint16_t pages;
  {
    Section baseline(epub, 2, renderer);
    ASSERT_TRUE(layout.startBuild(baseline));
    ASSERT_TRUE(baseline.buildSomeMore(0));
    pages = baseline.pageCount;
    baselineHtml = bytes(html());
    baselineSection = bytes(sectionFile());
    ASSERT_FALSE(baselineHtml.empty());
    ASSERT_FALSE(baselineSection.empty());
    ASSERT_TRUE(baseline.clearCache());
  }
  std::filesystem::remove(html());
  // A stale interrupted output must never be appended to or promoted.
  std::ofstream(partialHtml()) << "stale partial HTML";
  Section fallback(epub, 2, renderer);
  ZipInflateFaults::reset(48 * 1024);
  ASSERT_TRUE(layout.startBuild(fallback));
  EXPECT_EQ(ZipInflateFaults::stats().windowRejects, 1U);
  EXPECT_EQ(ZipInflateFaults::stats().largeChunks, 2U);
  EXPECT_EQ(ZipInflateFaults::stats().smallChunks, 2U);
  EXPECT_EQ(ZipInflateFaults::stats().live, 0U);
  ZipInflateFaults::disable();
  EXPECT_EQ(bytes(html()), baselineHtml);
  EXPECT_FALSE(std::filesystem::exists(partialHtml()));
  ASSERT_TRUE(fallback.buildSomeMore(0));
  EXPECT_EQ(fallback.pageCount, pages);
  EXPECT_EQ(fallback.lastBuildFailure(), Section::BuildStep::None);
  EXPECT_EQ(bytes(sectionFile()), baselineSection);
  Section reopened(epub, 2, renderer);
  ASSERT_TRUE(layout.load(reopened));
  EXPECT_EQ(reopened.pageCount, pages);
  ASSERT_NE(reopened.loadPage(pages - 1), nullptr);
}

TEST_F(SectionInflateRetry, ReadBufferFailureDoesNotReduceChunks) {
  Section section(epub, 2, renderer);
  ZipInflateFaults::reset(static_cast<size_t>(-1), false, true);
  ASSERT_TRUE(layout.startBuild(section));
  EXPECT_EQ(ZipInflateFaults::stats().largeChunks, 3U);
  EXPECT_EQ(ZipInflateFaults::stats().smallChunks, 0U);
  EXPECT_EQ(ZipInflateFaults::stats().windowRejects, 0U);
  EXPECT_EQ(ZipInflateFaults::stats().live, 0U);
}

TEST_F(SectionInflateRetry, PermanentWindowFailureStopsAfterThreeCleanAttempts) {
  Section section(epub, 2, renderer);
  ZipInflateFaults::reset(static_cast<size_t>(-1), true);
  EXPECT_FALSE(layout.startBuild(section));
  EXPECT_EQ(ZipInflateFaults::stats().windowRejects, 3U);
  EXPECT_EQ(ZipInflateFaults::stats().largeChunks, 2U);
  EXPECT_EQ(ZipInflateFaults::stats().smallChunks, 4U);
  EXPECT_EQ(ZipInflateFaults::stats().live, 0U);
  EXPECT_EQ(section.lastBuildFailure(), Section::BuildStep::HtmlStream);
  EXPECT_EQ(section.lastBuildFailureDetail(), static_cast<uint8_t>(ZipFile::StreamError::Window));
  expectNoPromotedHtml();
}

TEST_F(SectionInflateRetry, FinalOpenFailureReportsWriteInsteadOfPriorWindow) {
  Section section(epub, 2, renderer);
  removeHtmlAfterSecondReject = TestFs::root + epub->getCachePath() + "/html";
  ZipInflateFaults::reset(static_cast<size_t>(-1), true, false, failThirdOpen);
  EXPECT_FALSE(layout.startBuild(section));
  EXPECT_EQ(ZipInflateFaults::stats().windowRejects, 2U);
  EXPECT_EQ(ZipInflateFaults::stats().live, 0U);
  EXPECT_EQ(section.lastBuildFailure(), Section::BuildStep::HtmlStream);
  EXPECT_EQ(section.lastBuildFailureDetail(), static_cast<uint8_t>(ZipFile::StreamError::Write));
  expectNoPromotedHtml();
}
}  // namespace
