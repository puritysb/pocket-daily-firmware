#include <gtest/gtest.h>

#include <climits>
#include <utility>

#include "pocket_daily/ContentPageLayout.h"

using PocketDaily::Content::contentPageLayout;

TEST(ContentPageLayout, X3AndX4PortraitLandscapeAndRotatedInsetsStayInsideViewport) {
  for (const auto [width, height] : {std::pair{528, 792}, {792, 528}, {480, 800}, {800, 480}}) {
    for (int rotation = 0; rotation < 4; ++rotation) {
      const int edges[]{10, 12, 14, 16};
      const int top = edges[rotation], right = edges[(rotation + 1) % 4];
      const int bottom = edges[(rotation + 2) % 4], left = edges[(rotation + 3) % 4];
      const auto box = contentPageLayout(width, height, top, right, bottom, left, 12, 8, 4, 24);
      ASSERT_TRUE(box.valid);
      EXPECT_GE(box.x, left);
      EXPECT_LE(box.x + box.width, width - right);
      EXPECT_GE(box.y, top);
      EXPECT_LE(box.footer + box.advance, height - bottom);
      int y = box.y + 2 * box.advance + box.gap;
      ASSERT_GE(box.remainingLines(y, 2), 2U);
      y += box.remainingLines(y, 6) * box.advance;
      y += box.remainingLines(y, 4) * box.advance;
      EXPECT_LE(y, box.footer);
      EXPECT_LE(y + box.imageHeight(y), box.footer);
      EXPECT_EQ(box.remainingLines(box.footer, 10), 0U);
    }
  }
}

TEST(ContentPageLayout, EmptyStateReservesTitleGapAndTwoBodyLines) {
  // Five line advances including footer, plus one inter-block gap.
  EXPECT_FALSE(contentPageLayout(400, 104, 0, 0, 0, 0, 0, 0, 5, 15).valid);
  const auto exact = contentPageLayout(400, 105, 0, 0, 0, 0, 0, 0, 5, 15);
  ASSERT_TRUE(exact.valid);
  EXPECT_EQ(exact.remainingLines(exact.y + 2 * exact.advance + exact.gap, 2), 2U);
}

TEST(ContentPageLayout, ExtremeMetricsAreRejectedBeforeOverflowOrDrawing) {
  for (const int extreme : {INT_MAX, INT_MAX / 2}) {
    EXPECT_FALSE(contentPageLayout(528, 792, 0, 0, 0, 0, extreme, 0, 4, 24).valid);
    EXPECT_FALSE(contentPageLayout(528, 792, 0, 0, 0, 0, 12, extreme, 4, 24).valid);
    EXPECT_FALSE(contentPageLayout(528, 792, 0, 0, 0, 0, 12, 8, extreme, 24).valid);
    EXPECT_FALSE(contentPageLayout(528, 792, 0, 0, 0, 0, 12, 8, 4, extreme).valid);
    EXPECT_FALSE(contentPageLayout(528, 792, extreme, extreme, extreme, extreme, 12, 8, 4, 24).valid);
  }
  EXPECT_FALSE(contentPageLayout(INT_MIN, INT_MAX, 0, 0, 0, 0, 0, 0, 0, 1).valid);
  EXPECT_FALSE(contentPageLayout(528, 792, -1, 0, 0, 0, 0, 0, 0, 24).valid);
  EXPECT_FALSE(contentPageLayout(528, 792, 0, 0, 0, 0, 0, 0, 0, 0).valid);
  EXPECT_FALSE(contentPageLayout(528, 792, 0, 0, 0, 0, 0, 0, 0, INT_MIN).valid);
}

TEST(ContentPageLayout, NegativeThemeSpacingKeepsLegacyZeroClampAndInvalidCursorsAreEmpty) {
  const auto box = contentPageLayout(528, 792, 0, 0, 0, 0, INT_MIN, INT_MIN, INT_MIN, 24);
  ASSERT_TRUE(box.valid);
  EXPECT_EQ(box.x, 0);
  EXPECT_EQ(box.y, 0);
  EXPECT_EQ(box.gap, 0);
  EXPECT_EQ(box.advance, 24);
  for (const int cursor : {INT_MIN, -1, box.footer, INT_MAX}) {
    EXPECT_EQ(box.remainingLines(cursor, UINT_MAX), 0U);
    EXPECT_EQ(box.imageHeight(cursor), 0);
  }
}
