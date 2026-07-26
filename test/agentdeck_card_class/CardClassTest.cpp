#include <gtest/gtest.h>

#include "src/agentdeck/card_class.h"

namespace {

TEST(CardClass, PermissionGateIsLive) {
  EXPECT_STREQ(AgentDeck::classifyCardActionClass(true, "awaiting_permission"), "live");
  EXPECT_STREQ(AgentDeck::classifyCardActionClass(true, "idle"), "live");
}

TEST(CardClass, AwaitingPromptWithoutRequestIdIsLive) {
  EXPECT_STREQ(AgentDeck::classifyCardActionClass(false, "awaiting_option"), "live");
  EXPECT_STREQ(AgentDeck::classifyCardActionClass(false, "awaiting_diff"), "live");
}

TEST(CardClass, StatusRowsAreInfo) {
  EXPECT_STREQ(AgentDeck::classifyCardActionClass(false, "idle"), "info");
  EXPECT_STREQ(AgentDeck::classifyCardActionClass(false, "processing"), "info");
  EXPECT_STREQ(AgentDeck::classifyCardActionClass(false, ""), "info");
  EXPECT_STREQ(AgentDeck::classifyCardActionClass(false, nullptr), "info");
}

TEST(CardClass, LivePredicateMatchesOnlyLive) {
  EXPECT_TRUE(AgentDeck::actionClassIsLive("live"));
  EXPECT_FALSE(AgentDeck::actionClassIsLive("info"));
  EXPECT_FALSE(AgentDeck::actionClassIsLive("day"));
  EXPECT_FALSE(AgentDeck::actionClassIsLive(""));
  EXPECT_FALSE(AgentDeck::actionClassIsLive(nullptr));
}

}  // namespace
