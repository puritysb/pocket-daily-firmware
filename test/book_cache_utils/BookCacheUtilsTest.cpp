#include <gtest/gtest.h>

#include "CacheTest.h"
#include "util/BookCacheUtils.h"
TEST(BookCacheMutation, NonBookDeletionDoesNotRecreateDirtyMarker) {
  cache_test::reset();
  clearBookCache("/.crosspoint/library.dirty");
  clearBookCache("/.crosspoint/state.json");
  clearBookCache("/update.bin");
  EXPECT_EQ(cache_test::dirty, 0);
  EXPECT_EQ(cache_test::epubCleared + cache_test::txtCleared + cache_test::xtcCleared, 0);
}
TEST(BookCacheMutation, SupportedBooksStillInvalidateIndexAndTheirOwnCache) {
  for (int kind = 0; kind < 4; ++kind) {
    cache_test::reset();
    switch (kind) {
      case 0:
        cache_test::epub = true;
        break;
      case 1:
        cache_test::xtc = true;
        break;
      case 2:
        cache_test::txt = true;
        break;
      case 3:
        cache_test::markdown = true;
        break;
    }
    clearBookCache("/owned-book");
    EXPECT_EQ(cache_test::dirty, 1) << kind;
    EXPECT_EQ(cache_test::epubCleared, kind == 0);
    EXPECT_EQ(cache_test::xtcCleared, kind == 1);
    EXPECT_EQ(cache_test::txtCleared, kind == 2);
  }
}
