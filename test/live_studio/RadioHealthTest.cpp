#include <gtest/gtest.h>

#include "network/RadioHealthPolicy.h"

namespace {

using PocketDaily::RadioHealth::Level;
using PocketDaily::RadioHealth::Policy;

TEST(RadioHealthPolicy, HealthyProbesNeverEscalate) {
  Policy policy;
  for (int i = 0; i < 50; i++) {
    EXPECT_EQ(policy.onProbe(true), Level::None);
  }
  EXPECT_EQ(policy.currentLevel(), Level::None);
}

TEST(RadioHealthPolicy, EscalatesToReconnectOnlyAndResetsOnSuccess) {
  Policy policy;
  EXPECT_EQ(policy.onProbe(false), Level::None);
  EXPECT_EQ(policy.onProbe(false), Level::None);
  EXPECT_EQ(policy.onProbe(false), Level::DriverReconnect);
  // The ladder deliberately stops here: deeper actions fight the activity's
  // Wi-Fi state machine or crash the Arduino stack (observed 2026-09-20).
  EXPECT_EQ(policy.onProbe(false), Level::None);
  EXPECT_EQ(policy.onProbe(false), Level::None);
  EXPECT_EQ(policy.onProbe(false), Level::DriverReconnect);
  // Any successful probe fully heals.
  EXPECT_EQ(policy.onProbe(true), Level::None);
  EXPECT_EQ(policy.currentLevel(), Level::None);
}

TEST(RadioHealthPolicy, ResetReturnsToCleanState) {
  Policy policy;
  policy.onProbe(false);
  policy.onProbe(false);
  policy.reset();
  EXPECT_EQ(policy.currentLevel(), Level::None);
  EXPECT_EQ(policy.onProbe(false), Level::None);  // strike one, not escalation
}

}  // namespace
