#pragma once
//
// card_class.h — M6 card validity classes (`actionClass`).
//
// Device-side mirror of the daemon's classification chokepoint
// (AgentDeck bridge/src/card-feed.ts classifySessionCard) so cards cached
// from the live WS feed — which carries no actionClass — get the same class
// the HTTP pull feed would have stamped:
//
//   live — must be answered against a live daemon (permission gates, awaiting
//          prompts). A cached copy greys out ("reconnect to act") and is never
//          answerable offline.
//   day  — valid all day; answers queue in the Outbox. No producer until the
//          M7 card modules; reserved so the grammar doesn't change then.
//   info — read-only status row; always valid to display with a sync age.
//
// Pure header (no Arduino deps) — host-tested in test/agentdeck_card_class/.
//
#include <cstring>

namespace AgentDeck {

inline const char* classifyCardActionClass(bool hasRequestId, const char* state) {
  if (hasRequestId) return "live";
  if (state && strncmp(state, "awaiting", 8) == 0) return "live";
  return "info";
}

inline bool actionClassIsLive(const char* actionClass) {
  return actionClass && strcmp(actionClass, "live") == 0;
}

}  // namespace AgentDeck
