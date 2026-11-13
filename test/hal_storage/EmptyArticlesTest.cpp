#include <gtest/gtest.h>

#include "pocket_daily/web/EmptyArticles.h"
namespace {
struct Directory {
  enum class DirectoryRead { Record, End, Error };
  static inline bool opens = true, folder = true;
  static inline DirectoryRead result = DirectoryRead::End;
  explicit operator bool() const { return opens; }
  bool isDirectory() const { return folder; }
  DirectoryRead openNextEntry(Directory&) { return result; }
};
struct Storage {
  bool available = true, present = true, removes = true;
  unsigned removals = 0;
  bool ready() const { return available; }
  bool exists(const char* path) const {
    EXPECT_STREQ(path, "/Articles");
    return present;
  }
  Directory open(const char* path) {
    EXPECT_STREQ(path, "/Articles");
    return {};
  }
  bool rmdir(const char* path) {
    EXPECT_STREQ(path, "/Articles");
    ++removals;
    return removes;
  }
};
class EmptyArticles : public testing::Test {
 protected:
  void SetUp() override {
    Directory::opens = Directory::folder = true;
    Directory::result = Directory::DirectoryRead::End;
  }
};
TEST_F(EmptyArticles, OnlyExistingEmptyDirectoryIsRemoved) {
  Storage s;
  EXPECT_EQ(PocketDaily::Web::removeEmptyArticles(s), 204);
  EXPECT_EQ(s.removals, 1u);
  s.present = false;
  EXPECT_EQ(PocketDaily::Web::removeEmptyArticles(s), 404);
  EXPECT_EQ(s.removals, 1u);
}
TEST_F(EmptyArticles, EveryEntryIncludingHiddenAndReadErrorsPreventRemoval) {
  Storage s;
  Directory::result = Directory::DirectoryRead::Record;
  EXPECT_EQ(PocketDaily::Web::removeEmptyArticles(s), 409);
  Directory::result = Directory::DirectoryRead::Error;
  EXPECT_EQ(PocketDaily::Web::removeEmptyArticles(s), 503);
  EXPECT_EQ(s.removals, 0u);
}
TEST_F(EmptyArticles, MissingStorageBadHandlesAndFilesFailClosed) {
  Storage s;
  s.available = false;
  EXPECT_EQ(PocketDaily::Web::removeEmptyArticles(s), 503);
  s.available = true;
  Directory::opens = false;
  EXPECT_EQ(PocketDaily::Web::removeEmptyArticles(s), 503);
  Directory::opens = true;
  Directory::folder = false;
  EXPECT_EQ(PocketDaily::Web::removeEmptyArticles(s), 409);
  EXPECT_EQ(s.removals, 0u);
}
TEST_F(EmptyArticles, ConcurrentEntryOrRemovalFailureDoesNotReportSuccess) {
  Storage s;
  s.removes = false;
  EXPECT_EQ(PocketDaily::Web::removeEmptyArticles(s), 409);
  EXPECT_EQ(s.removals, 1u);
}
}  // namespace
