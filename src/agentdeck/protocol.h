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

namespace AgentDeck {
namespace Protocol {

// Parse one inbound JSON frame and apply it to g_state (thread-safe via mutex).
void parseMessage(const char* json, size_t length);

}  // namespace Protocol
}  // namespace AgentDeck
