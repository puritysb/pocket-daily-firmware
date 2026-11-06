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
  EXPECT_EQ(longPressFromFile(3, true, false), 1);  // retired Bilingual Toggle → Disabled
  EXPECT_EQ(longPressFromFile(4, true, false), 3);  // Dictionary
  EXPECT_EQ(longPressFromFile(5, true, false), 4);  // Reader Menu
}

TEST(LongPressMigration, MarkedFileKeepsStockValuesAndRetiresBilingualToggle) {
  for (uint8_t v = 0; v <= 4; ++v) EXPECT_EQ(longPressFromFile(v, true, /*hasOrderMarker=*/true), v);
  EXPECT_EQ(longPressFromFile(5, true, /*hasOrderMarker=*/true), 1);
}

using PocketDaily::SettingsMigration::lineSpacingFromStockLineHeight;
using PocketDaily::SettingsMigration::writtenByStockCrossPoint;

TEST(StockCrossPointMigration, OnlyAFileWithoutEitherForkMarkerIsStock) {
  EXPECT_TRUE(writtenByStockCrossPoint(/*hasStartupApp=*/false, /*hasOrderMarker=*/false));
  EXPECT_FALSE(writtenByStockCrossPoint(true, false));
  EXPECT_FALSE(writtenByStockCrossPoint(false, true));
  EXPECT_FALSE(writtenByStockCrossPoint(true, true));
}

// Stock 2.0 LINE_COMPRESSION: 100, 110, 120, 130, 145, 160, 175 %.
TEST(StockCrossPointMigration, LineHeightMapsEachStepToTheNearestForkSpacing) {
  constexpr uint8_t expected[] = {0, 0, 1, 2, 3, 3, 3};
  for (uint8_t v = 0; v < sizeof(expected); ++v) EXPECT_EQ(lineSpacingFromStockLineHeight(v), expected[v]) << int(v);
  EXPECT_EQ(lineSpacingFromStockLineHeight(200), 3);
}

// Stock's own upgrade maps fork Tight/Normal/Wide/Extra Wide to 110/120/130/145
// (lineHeight 1..4); the inverse must return each one unchanged.
TEST(StockCrossPointMigration, LineHeightInvertsStocksOwnSpacingUpgrade) {
  for (uint8_t fork = 0; fork <= 3; ++fork) EXPECT_EQ(lineSpacingFromStockLineHeight(fork + 1), fork);
}
