#pragma once

#include <cstddef>
#include <cstdint>

// Network stability instrumentation (docs/network-stability-analysis.md).
// The radio can die in ways that take every diagnostic path with it, so the
// record lives on the SD card and survives. Always on in dev builds
// (ENABLE_DEV_NETWORK_DIAGNOSTICS); compiled out of release builds until the
// workstream closes.
namespace PocketDaily::NetHealth {

// Pure helpers (host-tested):
// 802.11 disconnect reason code -> short name for the log.
const char* reasonName(int reason);
// One heartbeat line: "H <uptime_s> <freeHeap> <maxBlock> <rssi>".
int formatHeartbeat(char* out, size_t cap, uint32_t uptimeS, uint32_t freeHeap, uint32_t maxBlock, int32_t rssi);
// One event line: "E <uptime_s> <tag> [detail]".
int formatEvent(char* out, size_t cap, uint32_t uptimeS, const char* tag, int detail);

#ifdef ENABLE_DEV_NETWORK_DIAGNOSTICS
// Device side. begin() installs the WiFi event hooks. tick() is called from
// the activity loop's existing 2 s Wi-Fi check and logs the heartbeat line
// every 60 s - no dedicated task, no extra stack. note() records one-off
// events (loop wifi-loss branch, send timeouts, abandon paths).
void begin();
void tick();
void note(const char* tag, int detail = 0);
// Streams the tail of /.crosspoint/net-health.log for the dev endpoint.
size_t tail(char* out, size_t cap);
#else
inline void begin() {}
inline void tick() {}
inline void note(const char*, int = 0) { (void)0; }
inline size_t tail(char*, size_t) { return 0; }
#endif

}  // namespace PocketDaily::NetHealth
