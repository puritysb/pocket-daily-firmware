#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "CrossPointState.h"
#include "HalStorage.h"
#include "RecentBooksStore.h"
#include "pocket_daily/ContentImageRenderer.h"
#include "pocket_daily/ContentPage.h"
#include "pocket_daily/PocketReadingShelf.h"
#include "pocket_daily/TextValidation.h"
#include "pocket_daily/home/HomeInputs.h"
namespace PocketDaily::ReadingShelf {
Selection testSelection;
bool load(Selection& out) {
  out = testSelection;
  return true;
}
}  // namespace PocketDaily::ReadingShelf

// Compile the real HomeInputs translation unit; only device services and the
// unrelated image painter are faked. A text snapshot must not invoke a painter.
namespace PocketDaily::Content {
int contentImageHeight(const GfxRenderer&, const ManifestSource&, int, int, int, int) {
  ADD_FAILURE() << "readOpenBook must not draw images";
  return 0;
}
bool drawContentImage(const GfxRenderer&, const ManifestSource&, int, int, int, int) {
  ADD_FAILURE() << "readOpenBook must not draw images";
  return false;
}
}  // namespace PocketDaily::Content

namespace {
using PocketDaily::Home::OpenBook;
using PocketDaily::Home::readOpenBook;

class HomeInputsTest : public testing::Test {
 protected:
  void SetUp() override {
    APP_STATE.openEpubPath.clear();
    RECENT_BOOKS.books.clear();
    FakeSD::reset();
    PocketDaily::ReadingShelf::testSelection = {};
  }
  void open(const std::string& title, const std::string& author) {
    APP_STATE.openEpubPath = "/Books/reading.epub";
    FakeSD::files[APP_STATE.openEpubPath] = {'P', 'K'};
    RECENT_BOOKS.books.push_back({APP_STATE.openEpubPath, title, author, ""});
  }
  static void expectText(const char* actual, const std::string& expected) {
    EXPECT_STREQ(actual, expected.c_str());
    EXPECT_TRUE(PocketDaily::Text::validUtf8(actual, strlen(actual), false));
  }
};

// Distinct real Korean syllables make prefix/offset corruption visible.
const std::string titlePrefix =
    "오늘도책을읽으며마음에남는문장을천천히기록하는조용한시간을갖자";                     // 31 syllables / 93 B
const std::string authorPrefix = "김민수박서연이도윤최지우정하준강수빈조예린윤시우한별";  // 26 syllables / 78 B

TEST_F(HomeInputsTest, KoreanTitleAndAuthorDropOnlyAnIncompleteFinalSyllable) {
  ASSERT_EQ(titlePrefix.size(), 93u);
  ASSERT_EQ(authorPrefix.size(), 78u);
  open(titlePrefix + "요", authorPrefix + "빛");
  OpenBook book{};
  ASSERT_EQ(readOpenBook(book), &RECENT_BOOKS.books.front());
  expectText(book.title, titlePrefix);    // 95-byte title limit cuts after two UTF-8 bytes.
  expectText(book.author, authorPrefix);  // 79-byte author limit cuts after one byte.
  EXPECT_TRUE(book.valid);
  EXPECT_EQ(book.percent, -1);
  EXPECT_EQ(RECENT_BOOKS.books.front().title, titlePrefix + "요");
  EXPECT_EQ(RECENT_BOOKS.books.front().author, authorPrefix + "빛");
}

TEST_F(HomeInputsTest, ExactFitKoreanTextIsNotShortened) {
  const auto title = std::string("AB") + titlePrefix;
  const auto author = std::string("C") + authorPrefix;
  ASSERT_EQ(title.size(), 95u);
  ASSERT_EQ(author.size(), 79u);
  open(title, author);
  OpenBook book{};
  ASSERT_NE(readOpenBook(book), nullptr);
  expectText(book.title, title);
  expectText(book.author, author);
}

TEST_F(HomeInputsTest, LongTitlesKeepTheirMeaningfulPrefixIncludingMixedWidthText) {
  const auto title = std::string("독서 기록: ") + titlePrefix + " 그리고 내일의 이야기";
  const auto author = std::string("글: ") + authorPrefix + " 옮김: 홍길동";
  open(title, author);
  OpenBook book{};
  ASSERT_NE(readOpenBook(book), nullptr);
  expectText(book.title, "독서 기록: 오늘도책을읽으며마음에남는문장을천천히기록하는조용한");
  expectText(book.author, "글: 김민수박서연이도윤최지우정하준강수빈조예린윤시우");
}

TEST_F(HomeInputsTest, KoreanFilenameFallbackAlsoStopsAtACompleteSyllable) {
  APP_STATE.openEpubPath = "/Books/" + titlePrefix + "요.epub";
  FakeSD::files[APP_STATE.openEpubPath] = {'P', 'K'};
  OpenBook book{};
  EXPECT_EQ(readOpenBook(book), nullptr);
  expectText(book.title, titlePrefix);
  expectText(book.author, "");
  EXPECT_TRUE(book.valid);
}

TEST_F(HomeInputsTest, ShortTextAndStoredProgressArePreserved) {
  open("한강의 책", "한강");
  char path[64];
  snprintf(path, sizeof(path), "/.crosspoint/epub_%u/progress.bin",
           static_cast<unsigned>(std::hash<std::string>{}(APP_STATE.openEpubPath)));
  FakeSD::files[path] = {0, 0, 0, 0, 0, 0, 42};
  OpenBook book{};
  ASSERT_NE(readOpenBook(book), nullptr);
  expectText(book.title, "한강의 책");
  expectText(book.author, "한강");
  EXPECT_EQ(book.percent, 42);
}

TEST_F(HomeInputsTest, EmptyReadingStateClearsBothFields) {
  OpenBook book{};
  strcpy(book.title, "이전 책");
  strcpy(book.author, "이전 저자");
  EXPECT_EQ(readOpenBook(book), nullptr);
  expectText(book.title, "");
  expectText(book.author, "");
  EXPECT_FALSE(book.valid);
  EXPECT_EQ(book.percent, -1);
}
}  // namespace

TEST_F(HomeInputsTest, OpenBookDeletedFromTheCardIsNoLongerShown) {
  open("Pride and Prejudice", "Jane Austen");
  FakeSD::files.erase(APP_STATE.openEpubPath);
  OpenBook book{};
  EXPECT_EQ(readOpenBook(book), nullptr);
  EXPECT_FALSE(book.valid);
  EXPECT_STREQ(book.title, "");
}

TEST_F(HomeInputsTest, WithoutAnOpenBookContinueReadingFallsBackToTheMostRecentBookOnTheCard) {
  // Stock Home shows the first recent book; Pocket Daily must show the same one.
  RECENT_BOOKS.books.push_back({"/Books/gone.epub", "Gone", "", ""});
  RECENT_BOOKS.books.push_back({"/Books/neko.epub", "吾輩は猫である", "夏目漱石", ""});
  FakeSD::files["/Books/neko.epub"] = {'P', 'K'};
  EXPECT_EQ(PocketDaily::Home::continueReadingPath(), "/Books/neko.epub");
  OpenBook book{};
  ASSERT_NE(readOpenBook(book), nullptr);
  EXPECT_TRUE(book.valid);
  expectText(book.title, "吾輩は猫である");
}

TEST_F(HomeInputsTest, CompanionOffsetWinsOverSavedGlanceAndCrossesMidnight) {
  PocketDaily::AppGlance::Snapshot glance{};
  glance.savedEpoch = 1794242880;
  glance.utcOffsetMinutes = 0;
  // 2026-11-09 16:48 UTC is 01:48 on November 10 in Korea.
  const auto clock = PocketDaily::Home::readDeskClock(glance, 1794242880, 540);
  const auto utc = PocketDaily::Home::readDeskClock(glance, 1794242880);
  EXPECT_EQ(clock.date, 20261110u);
  EXPECT_EQ(clock.minute, 108);
  EXPECT_EQ(utc.date, 20261109u);
  EXPECT_EQ(utc.minute, 1008);
}

TEST_F(HomeInputsTest, RecentShelfKeepsPathsAndProgressAlignedAndOmitsMissingBooks) {
  RECENT_BOOKS.books.push_back({"/missing.epub", "Missing", "", ""});
  for (int i = 0; i < 6; ++i) {
    const auto path = "/Books/" + std::to_string(i) + ".epub";
    RECENT_BOOKS.books.push_back({path, "Book " + std::to_string(i), "Author " + std::to_string(i), ""});
    FakeSD::files[path] = {'P', 'K'};
    const auto progress =
        "/.crosspoint/epub_" + std::to_string(static_cast<unsigned>(std::hash<std::string>{}(path))) + "/progress.bin";
    FakeSD::files[progress] = {0, 0, 0, 0, 0, 0, static_cast<uint8_t>(11 + i * 17)};
  }
  PocketDaily::Home::BookSummary books[PocketDaily::Home::RECENT_CAP];
  char paths[PocketDaily::Home::RECENT_CAP][PocketDaily::Home::RECENT_PATH_BYTES]{};
  ASSERT_EQ(PocketDaily::Home::readRecentBooks(books, paths), 4);
  for (int i = 0; i < 4; ++i) {
    EXPECT_STREQ(paths[i], ("/Books/" + std::to_string(i) + ".epub").c_str());
    EXPECT_STREQ(books[i].title, ("Book " + std::to_string(i)).c_str());
    EXPECT_EQ(books[i].percent, 11 + i * 17);
  }
}

TEST_F(HomeInputsTest, RecentCoverPathsStayAlignedAfterMissingBooksAndClearOldSlots) {
  RECENT_BOOKS.books = {{"/gone.epub", "Gone", "A", "/wrong.bmp"},
                        {"/one.epub", "One", "B", "/one.bmp"},
                        {"/two.epub", "Two", "C", "/missing.bmp"},
                        {"/three.epub", "Three", "D", "/three.bmp"}};
  for (const auto* path : {"/one.epub", "/two.epub", "/three.epub", "/one.bmp", "/three.bmp"})
    FakeSD::files[path] = {'x'};
  PocketDaily::Home::BookSummary books[PocketDaily::Home::RECENT_CAP];
  char paths[PocketDaily::Home::RECENT_CAP][PocketDaily::Home::RECENT_PATH_BYTES];
  char covers[PocketDaily::Home::RECENT_CAP][PocketDaily::Home::RECENT_PATH_BYTES];
  memset(paths, 'x', sizeof(paths));
  memset(covers, 'x', sizeof(covers));
  EXPECT_EQ(PocketDaily::Home::readRecentBooks(books, paths, covers), 3);
  EXPECT_STREQ(covers[0], "/one.bmp");
  EXPECT_STREQ(covers[1], "");
  EXPECT_STREQ(covers[2], "/three.bmp");
  EXPECT_STREQ(covers[3], "");
  EXPECT_STREQ(paths[3], "");
  EXPECT_STREQ(books[2].title, "Three");
}

TEST_F(HomeInputsTest, ManualShelfKeepsExactOrderAndCoverAssociationSkippingMissingBooks) {
  using namespace PocketDaily;
  auto& selection = ReadingShelf::testSelection;
  selection.automatic = false;
  selection.count = 3;
  strcpy(selection.paths[0], "/b.epub");
  strcpy(selection.paths[1], "/missing.epub");
  strcpy(selection.paths[2], "/a.epub");
  RECENT_BOOKS.books = {{"/a.epub", "Alpha", "A", "/a.bmp"},
                        {"/b.epub", "Beta", "B", "/b.bmp"},
                        {"/missing.epub", "Missing", "", ""},
                        {"/c.epub", "Unselected", "", ""}};
  for (const auto* path : {"/a.epub", "/b.epub", "/c.epub", "/a.bmp", "/b.bmp"}) FakeSD::files[path] = {1};
  Home::BookSummary books[Home::RECENT_CAP];
  char paths[Home::RECENT_CAP][Home::RECENT_PATH_BYTES], covers[Home::RECENT_CAP][Home::RECENT_PATH_BYTES];
  ASSERT_EQ(Home::readRecentBooks(books, paths, covers), 2);
  EXPECT_STREQ(books[0].title, "Beta");
  EXPECT_STREQ(paths[0], "/b.epub");
  EXPECT_STREQ(covers[0], "/b.bmp");
  EXPECT_STREQ(books[1].title, "Alpha");
  EXPECT_STREQ(paths[1], "/a.epub");
  EXPECT_STREQ(covers[1], "/a.bmp");
  EXPECT_EQ(selection.count, 3);
  selection.count = 0;
  EXPECT_EQ(Home::readRecentBooks(books, paths, covers), 0);
}

TEST_F(HomeInputsTest, CacheTemplatesResolvePerBookToFullCoverOrConfiguredThumbnail) {
  using namespace PocketDaily;
  RECENT_BOOKS.books = {{"/a.epub", "Full", "", "/cache/a/thumb_[HEIGHT].bmp"},
                        {"/b.epub", "Thumb", "", "/cache/b/thumb_[HEIGHT].bmp"},
                        {"/c.epub", "Oversize full", "", "/cache/c/thumb_[HEIGHT].bmp"},
                        {"/d.epub", "Missing", "", "/cache/d/thumb_[HEIGHT].bmp"}};
  for (const auto* path : {"/a.epub", "/b.epub", "/c.epub", "/d.epub"}) FakeSD::files[path] = {1};
  FakeSD::files["/cache/a/cover.bmp"] = {1, 2, 3};
  FakeSD::files["/cache/a/thumb_320.bmp"] = {4};
  FakeSD::files["/cache/b/thumb_320.bmp"] = {5, 6};
  FakeSD::files["/cache/c/cover.bmp"] = std::vector<uint8_t>(131073, 1);
  FakeSD::files["/cache/c/thumb_320.bmp"] = {7, 8, 9, 10};
  // Other-size or empty cache files must not be mistaken for the configured image.
  FakeSD::files["/cache/d/thumb_200.bmp"] = {11};
  FakeSD::files["/cache/d/thumb_320.bmp"] = {};
  Home::BookSummary books[Home::RECENT_CAP];
  char covers[Home::RECENT_CAP][Home::RECENT_PATH_BYTES];
  ASSERT_EQ(Home::readRecentBooks(books, nullptr, covers), 4);
  EXPECT_STREQ(covers[0], "/cache/a/cover.bmp");
  EXPECT_STREQ(covers[1], "/cache/b/thumb_320.bmp");
  EXPECT_STREQ(covers[2], "/cache/c/thumb_320.bmp");
  EXPECT_STREQ(covers[3], "");
  for (unsigned i = 0; i < 4; ++i) {
    char resolved[Home::RECENT_PATH_BYTES];
    EXPECT_EQ(Home::resolveBookCoverPath(RECENT_BOOKS.books[i], resolved, sizeof(resolved)), i < 3);
    EXPECT_STREQ(resolved, covers[i]);
  }
  char tiny[8] = "stale";
  EXPECT_FALSE(Home::resolveBookCoverPath(RECENT_BOOKS.books[0], tiny, sizeof(tiny)));
  EXPECT_STREQ(tiny, "");
}
