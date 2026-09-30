#include <gtest/gtest.h>

#include "src/pocket_daily/SettingsMigration.h"

using PocketDaily::SettingsMigration::longPressFromFile;

// Stock numbering: KOSync 0, Disabled 1, Bookmark 2, Dictionary 3, Reader Menu 4.
TEST(LongPressMigration, StockFileKeepsStockMeaning) {
  for (uint8_t v = 0; v <= 4; ++v) EXPECT_EQ(longPressFromFile(v, /*writtenByFork=*/false, false), v);
}

TEST(LongPressMigration, PreFixForkFileMapsInsertedOrderToAppendedOrder) {
  EXPECT_EQ(longPressFromFile(0, true, false), 0);  // KOSync
  EXPECT_EQ(longPressFromFile(1, true, false), 1);  // Disabled
  EXPECT_EQ(longPressFromFile(2, true, false), 2);  // Bookmark
  EXPECT_EQ(longPressFromFile(3, true, false), 5);  // Bilingual Toggle
  EXPECT_EQ(longPressFromFile(4, true, false), 3);  // Dictionary
  EXPECT_EQ(longPressFromFile(5, true, false), 4);  // Reader Menu
}

TEST(LongPressMigration, MarkedFileIsAlreadyInAppendedOrder) {
  for (uint8_t v = 0; v <= 5; ++v) EXPECT_EQ(longPressFromFile(v, true, /*hasOrderMarker=*/true), v);
}
