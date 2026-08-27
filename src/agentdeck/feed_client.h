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

#include "endpoint_candidates.h"

namespace AgentDeck {
namespace Feed {

struct SyncResult {
  bool ok = false;
  // Conditional pull: the daemon confirmed the cached deck is still current —
  // nothing was parsed or persisted, re-sleep immediately.
  bool unchanged = false;
  // Daemon-suggested seconds until the next pull (its half of the power
  // ladder); 0 when the daemon didn't say — callers apply their default.
  uint32_t nextPullSec = 0;
  // Content signature of the applied feed (echo on the next pull). Empty on
  // failure; on `unchanged` it repeats the echoed sig.
  char deckSig[12] = {0};
  // Staged firmware advertised by the daemon (contract § Pull OTA). size 0 =
  // none. The caller decides whether to download/install (OtaPull::tryInstall).
  uint32_t fwSize = 0;
  char fwMd5[36] = {0};
  // Endpoint that actually completed the request after any daemon redirect.
  // Callers pass this directly to OTA and cache it as the next preferred path.
  char endpointIp[16] = {0};
};

// Telemetry appended to the `GET /feed` query string — the only battery/link
// observability a wake-sync-sleep device has. Negative/zero fields are omitted
// from the request (rssi is dBm, so "none" is 0, not -1).
struct SyncTelemetry {
  int battPct = -1;  // 0-100
  int rssiDbm = 0;   // negative when known
};

// Push pending outbox records (if any), then GET /feed and apply it to
// g_state. `board` is the wire identity ("xteink_x3"/"xteink_x4"); `echoSig`
// (nullable) is the deckSig persisted with the deck cache — when the daemon's
// current signature matches, it answers `unchanged` and the whole visit costs
// one tiny response. Blocking (HTTP on the loop task) — callers budget for a
// few seconds.
SyncResult syncOnce(const char* ip, uint16_t port, const char* token, const char* board, const char* echoSig,
                    const SyncTelemetry& telemetry);

// ── Endpoint cache ──
// The daemon endpoint is otherwise RAM-only (rediscovered every boot); a pull
// wake shouldn't spend its battery window on mDNS when the daemon rarely
// moves. The cached ip:port can still go stale across a daemon restart
// (dynamic 9120→9139), so callers must fall back to discovery on failure.
bool loadEndpoint(char* ip, size_t ipCap, uint16_t& port, char* token, size_t tokenCap);
bool saveEndpoint(const char* ip, uint16_t port, const char* token);
// Multi-interface form used by Pocket Daily. ADE1 single-address records are
// accepted and promoted to ADE2 on the next successful save.
bool loadEndpointCandidates(Net::EndpointCandidates& endpoints, char* token, size_t tokenCap);
// mergeWithCached keeps the learned order in front and lets the caller's set
// only contribute unseen addresses — the right behaviour for a discovery
// result. saveEndpoint() passes false because its set already carries the
// promotion of an address that just proved it works; merging there would put
// the stale order back in front and undo the very thing being recorded.
bool saveEndpointCandidates(const Net::EndpointCandidates& endpoints, const char* token, bool mergeWithCached = true);

// Reorder candidates so the first one is an address that currently accepts a
// TCP connection, and return true when the head changed.
//
// A dual-homed daemon host publishes every address it owns, but only some of
// them are actually usable from this device, and ICMP does not tell them apart
// — on the network this was written for, the dead address answers ping with 0%
// loss while every TCP pull to it fails. Probing the real thing (a connect to
// the service port) is the only honest test, and at a ~1.2 s budget it is far
// cheaper than discovering the same fact through a full HTTP timeout per
// candidate. Safe by construction: if nothing answers, the order is left alone
// and the normal try-every-candidate path still runs.
bool orderByReachability(Net::EndpointCandidates& endpoints, uint32_t probeTimeoutMs = 1200);

}  // namespace Feed
}  // namespace AgentDeck
