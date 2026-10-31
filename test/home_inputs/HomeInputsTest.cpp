#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "CrossPointState.h"
#include "HalStorage.h"
#include "RecentBooksStore.h"
#include "pocket_daily/ContentImageRenderer.h"
#include "pocket_daily/ContentPage.h"
#include "pocket_daily/TextValidation.h"
#include "pocket_daily/home/HomeInputs.h"

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
