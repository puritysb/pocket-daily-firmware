#pragma once

#include <cstdint>
#include <cstring>

namespace AgentDeck {

// Decision affordance for one awaiting session. This is deliberately pure and
// allocation-free so every X3/X4 surface follows the same honesty rule.
enum class AttentionMode : uint8_t {
  None = 0,
  PermissionGate,  // valid requestId: device-native binary allow/deny
  RealOptions,     // options[] correlated to this focused managed session
  WaitingForOptions,
  RespondInTerminal,
};

inline bool isObservedSession(const char* controlMode, const char* sessionId) {
  return (controlMode && strcmp(controlMode, "observed") == 0) ||
         (sessionId && strncmp(sessionId, "observed:", 9) == 0);
}

inline AttentionMode classifyAttention(bool awaiting, bool observed, bool hasRequestId, bool optionsCorrelated,
                                       uint8_t optionCount) {
  if (!awaiting) return AttentionMode::None;
  if (hasRequestId) return AttentionMode::PermissionGate;
  if (observed) return AttentionMode::RespondInTerminal;
  if (optionsCorrelated && optionCount > 0) return AttentionMode::RealOptions;
  return AttentionMode::WaitingForOptions;
}

inline bool attentionIsActionable(AttentionMode mode) {
  return mode == AttentionMode::PermissionGate || mode == AttentionMode::RealOptions;
}

}  // namespace AgentDeck
