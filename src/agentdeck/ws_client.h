#pragma once
//
// ws_client.h — port of AgentDeck esp32/src/net/ws_client.{h,cpp}.
//
// Thin WebSocket client (links2004/WebSockets) to the AgentDeck daemon. Driven
// cooperatively from the dashboard activity's loop() — NO dedicated FreeRTOS
// network task (see DESIGN constraint 2). The thread-safe outbox is retained so
// the M3 button handlers (which run on the render task / input path) can enqueue
// outbound commands safely.
//
#include <cstdint>

namespace AgentDeck {
namespace Net {

// Initialize the WS client (creates the outbox mutex). Does not connect.
void wsInit();

// Connect to the daemon WebSocket. An empty discovery token falls back to the
// NVS-provisioned credential. `board` is reported in the handshake so the
// daemon can identify and cable-free-adopt an unpaired X3/X4.
void wsConnect(const char* ip, uint16_t port, const char* token, const char* board);

// Disconnect and stop the reconnect loop.
void wsDisconnect();

// Request a disconnect after the current ws.loop() callback has unwound.
// WebSocketsClient is not safe to tear down recursively from its TEXT event.
void wsDisconnectAfterLoop();

// Pump WebSocket events + drive exponential-backoff reconnect. Call every loop.
void wsLoop();

bool wsConnected();
bool wsConnecting();

// Send JSON immediately (only when connected). Call from the loop/main task.
void wsSend(const char* json);

// Thread-safe enqueue from any task. Drained by pumpOutbound() on the loop task.
void queueOutbound(const char* json);

// Drain the outbox (WS when connected). Call every loop.
void pumpOutbound();

// Last-known bridge endpoint (for render / diagnostics / endpoint cache).
const char* wsBridgeIp();
uint16_t wsBridgePort();
const char* wsBridgeToken();

}  // namespace Net
}  // namespace AgentDeck
