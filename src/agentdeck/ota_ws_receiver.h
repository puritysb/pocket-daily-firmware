#pragma once
//
// ota_ws_receiver.h — AgentDeck WiFi OTA v1 client (daemon → device over the
// bridge WS socket).
//
// Wire contract (AgentDeck shared/src/protocol.ts, mirrored from the reference
// implementation in AgentDeck esp32/src/net/protocol.cpp):
//   inbound  esp32_ota_begin {otaId, size, md5}
//            esp32_ota_chunk {otaId, seq, offset, data(base64, 1024B decoded)}
//            esp32_ota_end   {otaId}
//            esp32_ota_abort {otaId}
//   outbound esp32_ota_ack   {otaId, stage, seq?, offset, written}
//            esp32_ota_error {otaId?, stage, error}
// The daemon sends strictly sequentially and waits for each ack (30s budget),
// resending the in-flight frame once on a mid-transfer WS reconnect — so state
// here must survive a socket drop, and a resend of the last-completed chunk is
// acked idempotently.
//
// CRITICAL fork difference from the reference: the X3/X4 must NOT flash through
// the Arduino Update class (esp_image_verify rejects the patched X4 image — see
// src/network/FirmwareFlasher.h). Chunks are therefore streamed to an SD cache
// file; on `end` the image is MD5- and structure-validated, the end-ack is sent
// (inside the daemon's 30s window), and the actual raw-partition flash + otadata
// switch + restart run afterwards from the main loop (serviceFlash()).
//
#include <cstddef>
#include <cstdint>

namespace AgentDeck {
namespace OtaWs {

// Consume an inbound WS text frame when it is an esp32_ota_* message.
// Returns true when consumed (caller must skip the normal protocol parser).
// Cheap for non-OTA frames: a strstr probe + a size gate before any JSON work.
bool maybeHandleFrame(const char* json, size_t length);

// A transfer is in progress (begin..end). The dashboard shows progress and must
// not exit (leaving the activity tears down the WS mid-transfer).
bool receiving();

// end received + image validated + ack sent: the heavy flash must now run on
// the main loop task, outside the WS event callback.
bool flashPending();

// Blocking: raw-partition flash + otadata switch + ESP.restart() on success.
// On failure sends esp32_ota_error, cleans up, and returns (device keeps
// running the current firmware — otadata only switches after a full write).
void serviceFlash();

// Progress for status lines (receive phase).
uint32_t receivedBytes();
uint32_t totalBytes();

}  // namespace OtaWs
}  // namespace AgentDeck
