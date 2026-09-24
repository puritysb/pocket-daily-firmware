#include <gtest/gtest.h>

#include <string>

#include "pocket_daily/ContentManifest.h"
#include "pocket_daily/ContentPathPolicy.h"

using PocketDaily::Content::genericContentWriteAllowed;

TEST(ContentPathPolicy, StagingIngressBudgetCoversAliasesAndTemporaryFilesOnlyWithinItsTree) {
  using namespace PocketDaily::Content;
  for (const char* path : {"/pocket-daily/content-staging", "/pocket-daily/content-staging/",
                           "/POCKET-DAILY/CONTENT-STAGING/revision/.pocket-123.part",
                           "/pocket-daily/content-staging/revision/image.pbm.davtmp"}) {
    EXPECT_TRUE(isContentStagingPath(path));
    EXPECT_TRUE(contentWriteWithinBudget(path, 0, MAX_CONTENT_BYTES));
    EXPECT_FALSE(contentWriteWithinBudget(path, 0, uint64_t(MAX_CONTENT_BYTES) + 1));
  }
  for (const char* path : {"/book.epub", "/update.bin", "/pocket-daily/content-staging-old/file",
                           "/other/pocket-daily/content-staging/file", "/pocket-daily/ui-packs/pack.uipack"}) {
    EXPECT_FALSE(isContentStagingPath(path));
    EXPECT_TRUE(contentWriteWithinBudget(path, 0, 16 * 1024 * 1024));
  }
}

TEST(ContentPathPolicy, StagingBudgetCountsResumedPrefixAndRejectsIntegerOverflow) {
  using namespace PocketDaily::Content;
  EXPECT_TRUE(contentStagingRangeAllowed(MAX_CONTENT_BYTES, 0));
  EXPECT_TRUE(contentStagingRangeAllowed(MAX_CONTENT_BYTES - 1, 1));
  EXPECT_FALSE(contentStagingRangeAllowed(MAX_CONTENT_BYTES - 1, 2));
  EXPECT_FALSE(contentStagingRangeAllowed(MAX_CONTENT_BYTES, 1));
  EXPECT_FALSE(contentStagingRangeAllowed(uint64_t(MAX_CONTENT_BYTES) + 1, 0));
  EXPECT_FALSE(contentStagingRangeAllowed(UINT64_MAX, 1));
  EXPECT_FALSE(contentStagingRangeAllowed(1, UINT64_MAX));
  uint64_t received = 0;
  while (received < MAX_CONTENT_BYTES) {
    ASSERT_TRUE(contentStagingRangeAllowed(received, 4096));
    received += 4096;
  }
  EXPECT_FALSE(contentStagingRangeAllowed(received, 4096));
}

TEST(ContentPathPolicy, ProtectsPublishedTreesRecordsAndAncestors) {
  for (const auto* path : {"/", "/pocket-daily", "/pocket-daily/", "/pocket-daily/content", "/pocket-daily/content/",
                           "/pocket-daily/content/revision/card.card", "/.crosspoint", "/.crosspoint/content-active.0",
                           "/.crosspoint/content-active.1", "/.crosspoint/content-active.0/child",
                           "/POCKET-DAILY/CONTENT/a", "/.CrossPoint/Content-Active.1"}) {
    EXPECT_FALSE(genericContentWriteAllowed(path)) << path;
  }
}

TEST(ContentPathPolicy, PreservesOrdinaryTransfersAndDistinctStagingTree) {
  for (const auto* path :
       {"/book.epub", "/Books/책.epub", "/Books/", "/update.bin", "/.pocket-transfer.part",
        "/pocket-daily/content-staging/revision/manifest.pdcm", "/pocket-daily/ui-packs/theme.bin",
        "/pocket-daily/learning/lesson.pdl", "/pocket-daily/content-other/card.card", "/.crosspoint/content-active.10",
        "/.crosspoint/content-active.0.backup", "/pocket-daily-other/content", "/books/pocket-daily/content"}) {
    EXPECT_TRUE(genericContentWriteAllowed(path)) << path;
  }
}

TEST(ContentPathPolicy, RejectsAmbiguousOrInvalidSegments) {
  for (const auto* path : {"",
                           "relative/file",
                           "//books/file",
                           "/books//file",
                           "/books/./file",
                           "/books/../file",
                           "/books/file.",
                           "/books /file",
                           "/books//",
                           "/POCKET~1/CONTENT/a",
                           "/.CROSS~1/CONTEN~1.0",
                           "/books/back\\slash",
                           "/books/colon:name",
                           "/books/*",
                           "/books/?",
                           "/books/\"",
                           "/books/<",
                           "/books/>",
                           "/books/|",
                           "/books/\nfile"}) {
    EXPECT_FALSE(genericContentWriteAllowed(path)) << path;
  }
}

TEST(ContentPathPolicy, RejectsEveryControlByteIncludingEmbeddedNul) {
  for (unsigned int c = 0; c <= 127; ++c) {
    if (c >= 32 && c != 127) continue;
    std::string path = "/books/a";
    path += static_cast<char>(c);
    path += "b.epub";
    EXPECT_FALSE(genericContentWriteAllowed(path)) << c;
  }
}

TEST(ContentPathPolicy, UsesViewLengthWithoutRequiringTerminator) {
  constexpr char path[] = "/pocket-daily/content-other";
  EXPECT_TRUE(genericContentWriteAllowed(path));
  EXPECT_FALSE(genericContentWriteAllowed(std::string_view(path, 21)));
  constexpr char book[] = {'/', 'b', 'o', 'o', 'k'};
  EXPECT_TRUE(genericContentWriteAllowed(std::string_view(book, sizeof(book))));
}
