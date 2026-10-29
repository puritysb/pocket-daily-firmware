#include <gtest/gtest.h>

#include <limits>
#include <vector>

#include "Epub.h"
#include "LibraryBuilder.h"
#include "LibraryIndexFile.h"

using namespace library;

namespace {
constexpr char INDEX[] = "/.crosspoint/library.idx";

class DevBuildBudgetTest : public ::testing::Test {
 protected:
  BuildStats stats;
  std::vector<uint8_t> oldIndex;

  void SetUp() override { prepare(); }
  void TearDown() override { endDevBuildBudget(); }

  void prepare() {
    endDevBuildBudget();
    fake::reset();
    bookMetadata.clear();
    fake::add("/a.epub");
    fake::add("/folder/b.epub");
    ASSERT_TRUE(buildLibraryIndex("/", stats, true));
    oldIndex = fake::files[INDEX]->bytes;
    fake::add("/new.epub");
    fake::add("/.crosspoint/recent.json", "preserve recent");
    fake::add("/.crosspoint/settings.bin", "preserve settings");
    ASSERT_TRUE(markLibraryIndexDirty());
    fake::parses = 0;
  }

  void expectPreserved() {
    ASSERT_TRUE(fake::files.count(INDEX));
    EXPECT_EQ(fake::files[INDEX]->bytes, oldIndex);
    EXPECT_FALSE(stats.indexReplaced);
    EXPECT_TRUE(isLibraryIndexDirty());
    const auto& recent = fake::files["/.crosspoint/recent.json"]->bytes;
    const auto& settings = fake::files["/.crosspoint/settings.bin"]->bytes;
    EXPECT_EQ(std::string(recent.begin(), recent.end()), "preserve recent");
    EXPECT_EQ(std::string(settings.begin(), settings.end()), "preserve settings");
    EXPECT_FALSE(fake::files.count("/.crosspoint/library.bak"));
  }
};
}  // namespace

TEST_F(DevBuildBudgetTest, RejectsInvalidAndNestedAdmissionWithoutReplacingActiveLimits) {
  EXPECT_FALSE(beginDevBuildBudget(0, 1, 1));
  EXPECT_FALSE(beginDevBuildBudget(1, 0, 1));
  EXPECT_FALSE(beginDevBuildBudget(1, 1, 0));
  EXPECT_FALSE(beginDevBuildBudget(4097, 1, 1));
  EXPECT_FALSE(beginDevBuildBudget(1, 131073, 1));
  EXPECT_FALSE(beginDevBuildBudget(1, 1, 45001));
  ASSERT_TRUE(beginDevBuildBudget(1, 131072, 45000));
  EXPECT_FALSE(beginDevBuildBudget(4096, 131072, 45000));
  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(endDevBuildBudget().entries, 1u);
  expectPreserved();
}

TEST_F(DevBuildBudgetTest, CountsNonBookAndHiddenDirectoryEntriesToo) {
  fake::add("/00-not-a-book.bin");
  ASSERT_TRUE(beginDevBuildBudget(1, 131072, 45000));
  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  const auto result = endDevBuildBudget();
  EXPECT_EQ(result.entries, 1u);
  EXPECT_TRUE(result.exhausted);
  EXPECT_FALSE(result.active);
  EXPECT_EQ(fake::parses, 0u);
  expectPreserved();
}

TEST_F(DevBuildBudgetTest, EveryWorkBudgetAbortPreservesThePreviouslyInstalledIndex) {
  ASSERT_TRUE(beginDevBuildBudget(4096, 131072, 45000));
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  const uint32_t work = endDevBuildBudget().workUnits;
  ASSERT_GT(work, 20u);  // includes prior scan, walk, reconciliation and emit
  for (uint32_t limit = 1; limit < work; ++limit) {
    SCOPED_TRACE(limit);
    prepare();
    ASSERT_TRUE(beginDevBuildBudget(4096, limit, 45000));
    EXPECT_FALSE(buildLibraryIndex("/", stats, true));
    const auto result = endDevBuildBudget();
    EXPECT_EQ(result.workUnits, limit);
    EXPECT_TRUE(result.exhausted);
    expectPreserved();
  }
}

TEST_F(DevBuildBudgetTest, DeadlineIsCheckedAfterOneUninterruptibleMetadataCall) {
  fake::metadataDelayMs = 1000;
  ASSERT_TRUE(beginDevBuildBudget(4096, 131072, 100));
  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  const auto result = endDevBuildBudget();
  EXPECT_TRUE(result.exhausted);
  EXPECT_GE(result.elapsedMs, 1000u);
  EXPECT_EQ(fake::parses, 1u);
  expectPreserved();
}

TEST_F(DevBuildBudgetTest, ElapsedDeadlineWorksAcrossMillisWrapAndRemainsLatched) {
  fake::clockMs = std::numeric_limits<uint32_t>::max() - 10;
  fake::metadataDelayMs = 1000;
  ASSERT_TRUE(beginDevBuildBudget(4096, 131072, 100));
  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  const auto result = endDevBuildBudget();
  EXPECT_TRUE(result.exhausted);
  EXPECT_GE(result.elapsedMs, 1000u);
  EXPECT_LT(result.elapsedMs, 2000u);
  EXPECT_TRUE(devBuildBudgetExhausted());
  EXPECT_EQ(devBuildBudgetSnapshot().elapsedMs, result.elapsedMs);
  expectPreserved();
}

TEST_F(DevBuildBudgetTest, UnchangedRebuildCannotClearDirtyMarkerAfterExpiredBudget) {
  fake::files.erase("/new.epub");
  ASSERT_TRUE(beginDevBuildBudget(4096, 131072, 1));
  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  EXPECT_TRUE(endDevBuildBudget().exhausted);
  expectPreserved();
}

TEST_F(DevBuildBudgetTest, DeadlineAfterFinalWritePreventsIndexInstallation) {
  fake::delayClosePath = "/.crosspoint/library.new";
  fake::closeDelayMs = 10000;
  ASSERT_TRUE(beginDevBuildBudget(4096, 131072, 5000));
  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  const auto result = endDevBuildBudget();
  EXPECT_TRUE(result.exhausted);
  EXPECT_GE(result.elapsedMs, 10000u);
  expectPreserved();
}

TEST_F(DevBuildBudgetTest, UnchangedIndexCloseCannotClearDirtyMarkerAfterDeadline) {
  fake::files.erase("/new.epub");
  fake::delayClosePath = INDEX;
  fake::closeDelayMs = 10000;
  ASSERT_TRUE(beginDevBuildBudget(4096, 131072, 5000));
  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  const auto result = endDevBuildBudget();
  EXPECT_TRUE(result.exhausted);
  EXPECT_GE(result.elapsedMs, 10000u);
  expectPreserved();
}

TEST_F(DevBuildBudgetTest, GenerousBudgetProducesTheSameIndexBytesAndCanBeReused) {
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  const auto expected = fake::files[INDEX]->bytes;
  prepare();
  ASSERT_TRUE(beginDevBuildBudget(4096, 131072, 45000));
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  const auto result = endDevBuildBudget();
  EXPECT_FALSE(result.exhausted);
  EXPECT_GT(result.entries, 0u);
  EXPECT_GT(result.workUnits, result.entries);
  EXPECT_EQ(fake::files[INDEX]->bytes, expected);
  EXPECT_FALSE(isLibraryIndexDirty());
  ASSERT_TRUE(beginDevBuildBudget(4096, 131072, 45000));
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_FALSE(endDevBuildBudget().exhausted);
  EXPECT_EQ(fake::files[INDEX]->bytes, expected);
}
