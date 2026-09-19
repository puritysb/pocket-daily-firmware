#pragma once

#include <cstddef>
#include <cstdint>

// Live Studio v1 event protocol (WebSocket text frames).
//
// This header is deliberately free of Arduino types so the wire grammar is
// exercised by the host test suite, following `upload_stream_protocol.h`.
// The contract is `docs/live-studio-v1.md`; the companion application is the
// consumer. Every message is a single-key JSON object:
//
//   Reader -> app: {"hello":{...}}, {"status":{...}}, {"prefs":{"changed":true}},
//               {"frame":{"seq":N,...}}   (LS-2), {"bye":{}}
//   App -> reader: {"subscribe":{"frames":bool,"minIntervalMs":N}},
//               {"unsubscribe":{}}, {"ping":{}}
//
// The listener runs only on STA with enough free heap; a reader that cannot
// run it advertises mode "poll" in /api/status and stays on HTTP.
namespace PocketDaily::LiveStudio {

inline constexpr char kProtocol[] = "live-studio/1";
inline constexpr uint32_t kMinStatusIntervalMs = 500;
inline constexpr uint32_t kKeepaliveIntervalMs = 15000;

// Capture gating for the LS-2 frame stream. Documented and host-tested with
// LS-1 so the policy is fixed before the capture path exists.
inline constexpr uint32_t kMinCaptureIntervalMs = 250;
inline constexpr uint32_t kMinCaptureFreeHeap = 10 * 1024;
// The WS listener itself needs a larger floor to start at all.
inline constexpr uint32_t kMinListenerFreeHeap = 40 * 1024;

struct Subscription {
  bool frames = false;  // LS-2: accepted, not yet served
  uint32_t minIntervalMs = kMinCaptureIntervalMs;
};

enum class ClientMessage : uint8_t { None, Subscribe, Unsubscribe, Ping };

// Encoders return false when the buffer is too small; output is always
// NUL-terminated on success.
bool encodeHello(char* out, size_t cap, const char* deviceId, const char* version);
bool encodePong(char* out, size_t cap);
bool encodeBye(char* out, size_t cap);
bool encodePrefsChanged(char* out, size_t cap);
// `statusJson` (the exact /api/status body) is embedded verbatim.
bool encodeStatusEvent(char* out, size_t cap, const char* statusJson);

// Deterministic scanner for the app -> reader messages. Unknown or malformed
// input decodes to ClientMessage::None; `out` (optional) receives the
// clamped subscription on Subscribe.
ClientMessage parseClientMessage(const char* payload, size_t length, Subscription* out);

// Pure capture gate for the LS-2 frame stream: heap floor plus minimum
// spacing since the previous capture, using wrap-safe millisecond math.
bool shouldCaptureFrame(uint32_t freeHeap, uint32_t nowMs, uint32_t lastCaptureMs);

}  // namespace PocketDaily::LiveStudio
