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

TEST(RadioHealthPolicy, EscalatesAfterThreeDeafStrikesAndResetsOnSuccess) {
  Policy policy;
  EXPECT_EQ(policy.onProbe(false), Level::None);
  EXPECT_EQ(policy.onProbe(false), Level::None);
  EXPECT_EQ(policy.onProbe(false), Level::DriverReconnect);
  EXPECT_EQ(policy.currentLevel(), Level::DriverReconnect);
  // Strikes reset per escalation; three more dead probes go one level up.
  EXPECT_EQ(policy.onProbe(false), Level::None);
  EXPECT_EQ(policy.onProbe(false), Level::None);
  EXPECT_EQ(policy.onProbe(false), Level::FullReassociate);
  // Radio restart is the ceiling and repeats.
  EXPECT_EQ(policy.onProbe(false), Level::None);
  EXPECT_EQ(policy.onProbe(false), Level::None);
  EXPECT_EQ(policy.onProbe(false), Level::RadioRestart);
  EXPECT_EQ(policy.onProbe(false), Level::None);
  EXPECT_EQ(policy.onProbe(false), Level::None);
  EXPECT_EQ(policy.onProbe(false), Level::RadioRestart);
  // Any successful probe fully heals.
  EXPECT_EQ(policy.onProbe(true), Level::None);
  EXPECT_EQ(policy.currentLevel(), Level::None);
  EXPECT_EQ(policy.onProbe(false), Level::None);  // starts from strike one again
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
