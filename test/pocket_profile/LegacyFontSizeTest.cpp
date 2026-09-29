#include <gtest/gtest.h>

#include "pocket_daily/web/LegacyFontSize.h"

namespace Size = PocketDaily::Web::LegacyFontSize;

TEST(LegacyFontSize, ExistingAppSlotsKeepPhysicalSizes) {
  EXPECT_EQ(Size::toPoints(0), 12);
  EXPECT_EQ(Size::toPoints(1), 14);
  EXPECT_EQ(Size::toPoints(2), 16);
  EXPECT_EQ(Size::toPoints(3), 18);
  for (uint8_t slot = 0; slot < Size::COUNT; ++slot) EXPECT_EQ(Size::fromPoints(Size::toPoints(slot)), slot);
}

TEST(LegacyFontSize, NewDeviceSizesHaveBoundedLegacyRepresentation) {
  EXPECT_EQ(Size::fromPoints(8), 0);
  EXPECT_EQ(Size::fromPoints(13), 0);
  EXPECT_EQ(Size::fromPoints(15), 1);
  EXPECT_EQ(Size::fromPoints(17), 2);
  EXPECT_EQ(Size::fromPoints(48), 3);
  EXPECT_EQ(Size::toPoints(255), 14);
}
