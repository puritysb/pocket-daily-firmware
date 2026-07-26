#pragma once
//
// protocol.h — TRIMMED port of AgentDeck esp32/src/net/protocol.{h,cpp}.
//
// Parses inbound bridge JSON and updates g_state. M2 handles the message types
// a display-only client needs: state_update, sessions_list, usage_update, and
// the connection/connected acknowledgements. Other message types are accepted
// and ignored (see protocol.cpp for the M3-stubbed list).
//
// NOTE: esp32_ota_* frames never reach this parser — ws_client.cpp routes them
// to OtaWs::maybeHandleFrame() first, because this parser's ArduinoJson filter
// would silently drop the base64 chunk payload (`data`) and kill the transfer.
//
#include <cstddef>
#include <cstdint>

namespace AgentDeck {
namespace Protocol {

// Parse one inbound JSON frame and apply it to g_state (thread-safe via mutex).
void parseMessage(const char* json, size_t length);

// M6 pull sync: parse a `GET /feed` card_feed body and land the embedded
// sessions in g_state exactly like a sessions_list frame would, plus re-anchor
// the daemon clock estimate from `serverTime` (the drifty-RTC "as of" source).
// Contract: AgentDeck shared/src/protocol.ts § Card Feed Pull Sync.
// Returns false on oversize/parse/shape failure; *nextPullSecOut (when given)
// receives the daemon's suggested sleep interval, 0 when absent.
bool applyCardFeed(const char* json, size_t length, uint32_t* nextPullSecOut);

}  // namespace Protocol
}  // namespace AgentDeck
