#pragma once
//
// feed_client.h — M6 HTTP pull sync (wake-sync-sleep).
//
// The battery-mode counterpart to ws_client: one `GET /feed` refreshes the
// deck (landing in g_state via Protocol::applyCardFeed) and one `POST /outbox`
// drains locally recorded decisions — no persistent socket, so the device can
// wake, sync, and deep-sleep in seconds. WS remains the docked live mode.
// Contract: AgentDeck shared/src/protocol.ts § Card Feed Pull Sync +
// docs/esp32-client-contract.md § Pull sync.
//
#include <cstddef>
#include <cstdint>

namespace AgentDeck {
namespace Feed {

struct SyncResult {
  bool ok = false;
  // Daemon-suggested seconds until the next pull (its half of the power
  // ladder); 0 when the daemon didn't say — callers apply their default.
  uint32_t nextPullSec = 0;
};

// Push pending outbox records (if any), then GET /feed and apply it to
// g_state. `board` is the wire identity ("xteink_x3"/"xteink_x4").
// Blocking (HTTP on the loop task) — callers budget for a few seconds.
SyncResult syncOnce(const char* ip, uint16_t port, const char* token, const char* board);

// ── Endpoint cache ──
// The daemon endpoint is otherwise RAM-only (rediscovered every boot); a pull
// wake shouldn't spend its battery window on mDNS when the daemon rarely
// moves. The cached ip:port can still go stale across a daemon restart
// (dynamic 9120→9139), so callers must fall back to discovery on failure.
bool loadEndpoint(char* ip, size_t ipCap, uint16_t& port, char* token, size_t tokenCap);
bool saveEndpoint(const char* ip, uint16_t port, const char* token);

}  // namespace Feed
}  // namespace AgentDeck
