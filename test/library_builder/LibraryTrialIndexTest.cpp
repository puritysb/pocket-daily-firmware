#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "Epub.h"
#include "LibraryBuilder.h"
#include "LibraryIndexFile.h"

namespace {
constexpr char ORIGINAL[] = "/pocket-rc-original-ko.epub";
constexpr char OWNED[] = "/Pocket visual 123.epub";
constexpr char TITLE[] = "Pocket RC 한국어 읽기";
constexpr char INDEX[] = "/.crosspoint/library.idx";

struct Row {
  std::string path;
  std::string title;
  uint32_t bytes;
};

std::vector<Row> rows(library::LibraryIndexFile& index, library::SortOrder order) {
  std::vector<Row> result;
  for (uint16_t row = 0; row < index.bookCount(); ++row) {
    const auto ordinal = index.ordinalForRow(order, row);
    library::ClixRecord record{};
    Row value;
    if (ordinal == UINT16_MAX || !index.readRecord(ordinal, record) || !index.readPath(record, value.path) ||
        !index.readTitle(record, value.title))
      return {};
    value.bytes = record.fileSize;
    result.push_back(value);
  }
  return result;
}

class LibraryTrialIndexTest : public ::testing::Test {
 protected:
  void SetUp() override {
    fake::reset();
    bookMetadata.clear();
    fake::add(ORIGINAL, "identical original fixture bytes", 10);
    bookMetadata[ORIGINAL] = {TITLE, "Pocket Test", true};
    library::endDevBuildBudget();
  }
  void build() {
    ASSERT_TRUE(library::beginDevBuildBudget(4096, 131072, 45000));
    library::BuildStats stats;
    const bool ok = library::buildLibraryIndex("/", stats, true);
    const auto budget = library::endDevBuildBudget();
    ASSERT_TRUE(ok);
    ASSERT_FALSE(budget.exhausted);
  }
};
}  // namespace

// Metadata is a deterministic fixture here. The builder, CLIX serialization,
// path/title separation, sort permutation and lookup are real production code.
// Actual EPUB parse, Library widget paint and selection remain separate gates.
TEST_F(LibraryTrialIndexTest, KoreanOriginalAndIdenticalOwnedCopyRemainDistinctRows) {
  build();
  fake::add(OWNED, "identical original fixture bytes", 20);
  bookMetadata[OWNED] = {TITLE, "Pocket Test", true};
  ASSERT_TRUE(library::markLibraryIndexDirty());
  build();
  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  ASSERT_EQ(index.bookCount(), 2);
  EXPECT_FALSE(index.ranksDegraded());
  EXPECT_FALSE(index.dedupDegraded());
  for (const auto order : {library::SortOrder::RecentDesc, library::SortOrder::RecentAsc, library::SortOrder::TitleAsc,
                           library::SortOrder::TitleDesc}) {
    const auto found = rows(index, order);
    ASSERT_EQ(found.size(), 2u);
    EXPECT_NE(found[0].path, found[1].path);
    for (const auto& row : found) {
      EXPECT_TRUE(row.path == ORIGINAL || row.path == OWNED);
      EXPECT_EQ(row.title, TITLE);
      EXPECT_EQ(row.bytes, fake::files[ORIGINAL]->bytes.size());
    }
  }
  EXPECT_EQ(rows(index, library::SortOrder::RecentDesc)[0].path, OWNED);
}

TEST_F(LibraryTrialIndexTest, OwnedRemovalRebuildKeepsOriginalBytesTitleAndNoStaleRow) {
  fake::add(OWNED, "identical original fixture bytes", 20);
  bookMetadata[OWNED] = {TITLE, "Pocket Test", true};
  build();
  const auto original = fake::files[ORIGINAL]->bytes;
  ASSERT_TRUE(Storage.remove(OWNED));
  ASSERT_TRUE(library::markLibraryIndexDirty());
  build();
  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  ASSERT_EQ(index.bookCount(), 1);
  const auto found = rows(index, library::SortOrder::RecentDesc);
  ASSERT_EQ(found.size(), 1u);
  EXPECT_EQ(found[0].path, ORIGINAL);
  EXPECT_EQ(found[0].title, TITLE);
  EXPECT_EQ(fake::files[ORIGINAL]->bytes, original);
  EXPECT_FALSE(Storage.exists(OWNED));
  EXPECT_FALSE(library::isLibraryIndexDirty());
}

TEST_F(LibraryTrialIndexTest, MetadataTitleNeverBecomesTheOpenPath) {
  fake::add("/different.epub", "other bytes", 12);
  bookMetadata["/different.epub"] = {TITLE, "Pocket Test", true};
  build();
  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  const auto found = rows(index, library::SortOrder::TitleAsc);
  ASSERT_EQ(found.size(), 2u);
  for (const auto& row : found) {
    EXPECT_EQ(row.title, TITLE);
    EXPECT_NE(row.path, std::string("/") + TITLE);
    EXPECT_TRUE(Storage.exists(row.path.c_str()));
  }
}
