#include <gtest/gtest.h>

#include "src/agentdeck/attention_contract.h"

namespace {

using AgentDeck::AttentionMode;

TEST(AttentionContract, ObservedPromptWithoutRequestIdIsTerminalOnly) {
  EXPECT_EQ(AgentDeck::classifyAttention(true, true, false, false, 0), AttentionMode::RespondInTerminal);
  EXPECT_FALSE(AgentDeck::attentionIsActionable(AttentionMode::RespondInTerminal));
}

TEST(AttentionContract, RequestIdIsARealBinaryPermissionGate) {
  EXPECT_EQ(AgentDeck::classifyAttention(true, true, true, false, 0), AttentionMode::PermissionGate);
  EXPECT_TRUE(AgentDeck::attentionIsActionable(AttentionMode::PermissionGate));
}

TEST(AttentionContract, ManagedPromptWaitsUntilOptionsAreCorrelated) {
  EXPECT_EQ(AgentDeck::classifyAttention(true, false, false, false, 3), AttentionMode::WaitingForOptions);
  EXPECT_EQ(AgentDeck::classifyAttention(true, false, false, true, 0), AttentionMode::WaitingForOptions);
}

TEST(AttentionContract, CorrelatedManagedOptionsAreActionable) {
  EXPECT_EQ(AgentDeck::classifyAttention(true, false, false, true, 3), AttentionMode::RealOptions);
  EXPECT_TRUE(AgentDeck::attentionIsActionable(AttentionMode::RealOptions));
}

TEST(AttentionContract, NonAwaitingSessionHasNoAttentionAffordance) {
  EXPECT_EQ(AgentDeck::classifyAttention(false, true, true, true, 3), AttentionMode::None);
}

TEST(AttentionContract, ObservedPrefixIsAControlModeFallback) {
  EXPECT_TRUE(AgentDeck::isObservedSession("", "observed:claude:session-id"));
  EXPECT_TRUE(AgentDeck::isObservedSession("observed", "session-id"));
  EXPECT_FALSE(AgentDeck::isObservedSession("managed", "session-id"));
}

}  // namespace
