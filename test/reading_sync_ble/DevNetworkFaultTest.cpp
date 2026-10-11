#include <gtest/gtest.h>

#include "pocket_daily/web/DevNetworkFault.h"
using namespace PocketDaily::Web::DevNetworkFault;
TEST(DevNetworkFault, BindsSizePointAndVerifiedPrefix) {
  Trial t;
  ASSERT_TRUE(t.arm(1, 200000, Point::Receiving, 10));
  EXPECT_FALSE(t.trigger(Point::Published, 200000, 200000, 20));
  EXPECT_FALSE(t.trigger(Point::Receiving, 300000, 65536, 20));
  EXPECT_FALSE(t.trigger(Point::Receiving, 200000, 65535, 20));
  ASSERT_TRUE(t.trigger(Point::Receiving, 200000, 65536, 20));
  EXPECT_FALSE(t.trigger(Point::Receiving, 200000, 70000, 21));
  EXPECT_EQ(t.bytes, 65536u);
}
TEST(DevNetworkFault, RestoresOnceAfterActualOfflinePeriod) {
  Trial t;
  ASSERT_TRUE(t.arm(1, 200000, Point::Published, 10));
  ASSERT_TRUE(t.trigger(Point::Published, 200000, 200000, 20));
  EXPECT_FALSE(t.arm(2, 200000, Point::Published, 30));
  EXPECT_FALSE(t.tick(5019, false));
  EXPECT_TRUE(t.tick(5020, false));
  EXPECT_FALSE(t.tick(5021, false));
  EXPECT_FALSE(t.tick(6000, true));
  EXPECT_EQ(t.state, State::Complete);
  EXPECT_TRUE(t.sawOffline);
  EXPECT_EQ(t.restoredAt, 6000u);
}
TEST(DevNetworkFault, MissingOfflineObservationCannotImplyRadioLoss) {
  Trial t;
  ASSERT_TRUE(t.arm(1, 200000, Point::Published, 0));
  EXPECT_FALSE(t.trigger(Point::Published, 200000, 199999, 1));
  ASSERT_TRUE(t.trigger(Point::Published, 200000, 200000, 2));
  EXPECT_TRUE(t.tick(5002, true));
  t.tick(5003, true);
  EXPECT_FALSE(t.sawOffline);
}
TEST(DevNetworkFault, ExpiryAndBoundsPreventLateUnrelatedFaults) {
  Trial t;
  EXPECT_FALSE(t.arm(0, 200000, Point::Receiving, 0));
  EXPECT_FALSE(t.arm(1, 100, Point::Receiving, 0));
  EXPECT_FALSE(t.arm(1, 2097153, Point::Receiving, 0));
  ASSERT_TRUE(t.arm(1, 200000, Point::Receiving, UINT32_MAX - 100));
  EXPECT_FALSE(t.trigger(Point::Receiving, 200000, 65536, 60000));
  EXPECT_EQ(t.state, State::Expired);
}
