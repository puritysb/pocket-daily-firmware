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
//               {"bye":{}}
//   App -> reader: {"subscribe":{...}}, {"unsubscribe":{}}, {"ping":{}}
//
// Frame streaming (LS-2) was removed on 2026-10-01; older companions may still
// send {"subscribe":{"frames":true}}, which is a plain status subscription.
//
// The listener runs only on STA with enough free heap; a reader that cannot
// run it advertises mode "poll" in /api/status and stays on HTTP.
namespace PocketDaily::LiveStudio {

inline constexpr char kProtocol[] = "live-studio/1";
inline constexpr uint32_t kMinStatusIntervalMs = 500;
inline constexpr uint32_t kKeepaliveIntervalMs = 15000;

// The WS listener gate, checked AFTER the server settles: the heap map
// (2026-09-20) showed the DMA pool bottoming 3.5 KB from empty during
// transfers at an 8.7 KB settle, and the morning's stable build settled
// ~13.6 KB. The listener's ~3 KB must not ride on builds that settle this
// tight - below 16 KB at start, the reader stays poll-only.
inline constexpr uint32_t kMinListenerFreeHeap = 16 * 1024;

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
// input decodes to ClientMessage::None.
ClientMessage parseClientMessage(const char* payload, size_t length);

}  // namespace PocketDaily::LiveStudio
