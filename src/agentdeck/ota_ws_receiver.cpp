#include "ota_ws_receiver.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HalStorage.h>
#include <MD5Builder.h>
#include <esp_ota_ops.h>
#include <mbedtls/base64.h>

#include <cstring>

#include "agent/AgentLog.h"
#include "network/FirmwareFlasher.h"
#include "ws_client.h"

namespace AgentDeck {
namespace OtaWs {

namespace {

// SD cache the chunks stream into. Truncated on every `begin`; validated and
// flashed from `end`. Lives under the hidden settings dir.
constexpr const char* kCacheDir = "/.crosspoint";
constexpr const char* kCachePath = "/.crosspoint/agentdeck-ota.bin";

// Daemon chunks are 1024 raw bytes (base64 ~1368 chars, frame ~1.5 KB). Gate
// hard so a hostile/corrupt frame can't force a large decode. Non-OTA frames
// (sessions_list can be ~30 KB) are rejected by this gate before any JSON work.
constexpr size_t kMaxOtaFrameBytes = 8192;
constexpr size_t kMaxDecodedChunk = 4096;

struct RxState {
  bool active = false;
  bool flashPending = false;
  char otaId[40] = {0};
  char md5[36] = {0};
  uint32_t expectedSize = 0;
  uint32_t written = 0;
  uint32_t nextSeq = 0;
  uint32_t lastRxMs = 0;  // millis() of the last begin/chunk — drives the stall abort
};

// A receive with no traffic for this long is dead (daemon crashed/quit mid-push,
// network partition). Without this, rx.active stays set forever and the activity
// keeps swallowing all input "until the transfer resolves" — a soft brick until
// reboot. The daemon's own per-chunk ack timeout is ~10 s, so 30 s of silence
// can only mean the sender is gone; a later retry starts fresh via handleBegin.
constexpr uint32_t kRxStallTimeoutMs = 30000;

RxState rx;
HalFile cacheFile;
MD5Builder md5Builder;
uint8_t decodeBuf[kMaxDecodedChunk];

void sendAck(const char* otaId, const char* stage, uint32_t seq, uint32_t offset, uint32_t written) {
  char buf[192];
  int n;
  if (seq != UINT32_MAX)
    n = snprintf(buf, sizeof(buf),
                 "{\"type\":\"esp32_ota_ack\",\"otaId\":\"%s\",\"stage\":\"%s\",\"seq\":%u,\"offset\":%u,"
                 "\"written\":%u}",
                 otaId, stage, (unsigned)seq, (unsigned)offset, (unsigned)written);
  else
    n = snprintf(buf, sizeof(buf),
                 "{\"type\":\"esp32_ota_ack\",\"otaId\":\"%s\",\"stage\":\"%s\",\"offset\":%u,\"written\":%u}", otaId,
                 stage, (unsigned)offset, (unsigned)written);
  if (n > 0 && (size_t)n < sizeof(buf)) Net::wsSend(buf);
}

void sendError(const char* otaId, const char* stage, const char* error) {
  char buf[192];
  const int n = snprintf(buf, sizeof(buf), "{\"type\":\"esp32_ota_error\",\"otaId\":\"%s\",\"stage\":\"%s\",\"error\":\"%s\"}",
                         otaId ? otaId : "", stage, error);
  if (n > 0 && (size_t)n < sizeof(buf)) Net::wsSend(buf);
  AgentLog::line("OTA", "error stage=%s err=%s", stage, error);
}

void resetRx(bool removeFile) {
  if (cacheFile) cacheFile.close();
  if (removeFile) Storage.remove(kCachePath);
  rx = RxState{};
}

void handleBegin(JsonObjectConst obj) {
  const char* otaId = obj["otaId"] | "";
  const char* md5 = obj["md5"] | "";
  const uint32_t size = obj["size"] | 0U;
  if (!otaId[0] || size == 0) {
    sendError(otaId, "begin", "missing_parameters");
    return;
  }

  const esp_partition_t* dest = esp_ota_get_next_update_partition(nullptr);
  if (!dest) {
    sendError(otaId, "begin", "no_ota_partition");
    return;
  }
  if (size > dest->size) {
    sendError(otaId, "begin", "image_too_large");
    return;
  }

  if (rx.active) resetRx(true);  // daemon retried begin — restart cleanly

  Storage.mkdir(kCacheDir);
  Storage.remove(kCachePath);
  if (!Storage.openFileForWrite("OTA", kCachePath, cacheFile)) {
    sendError(otaId, "begin", "sd_open_failed");
    resetRx(false);
    return;
  }

  rx.active = true;
  rx.flashPending = false;
  strncpy(rx.otaId, otaId, sizeof(rx.otaId) - 1);
  rx.otaId[sizeof(rx.otaId) - 1] = '\0';
  strncpy(rx.md5, md5, sizeof(rx.md5) - 1);
  rx.md5[sizeof(rx.md5) - 1] = '\0';
  rx.expectedSize = size;
  rx.written = 0;
  rx.nextSeq = 0;
  rx.lastRxMs = millis();
  md5Builder.begin();
  AgentLog::line("OTA", "begin id=%s size=%u md5=%s", rx.otaId, (unsigned)size, rx.md5);
  sendAck(rx.otaId, "begin", UINT32_MAX, 0, 0);
}

void handleChunk(JsonObjectConst obj) {
  const char* otaId = obj["otaId"] | "";
  const uint32_t seq = obj["seq"] | UINT32_MAX;
  const uint32_t offset = obj["offset"] | 0U;
  const char* data = obj["data"] | "";
  if (!rx.active || strcmp(rx.otaId, otaId) != 0) {
    sendError(otaId, "chunk", "no_active_update");
    return;
  }
  rx.lastRxMs = millis();  // any chunk traffic (even a dup) proves the sender lives
  // The daemon may resend the last chunk after a WS reconnect when the ack was
  // lost after the write completed. Idempotent re-ack, no rewrite.
  if (seq + 1 == rx.nextSeq && offset < rx.written) {
    sendAck(otaId, "chunk", seq, offset, rx.written);
    return;
  }
  if (seq != rx.nextSeq || offset != rx.written) {
    sendError(otaId, "chunk", "unexpected_offset");
    return;
  }
  if (!data[0]) {
    sendError(otaId, "chunk", "missing_data");
    return;
  }

  size_t decodedLen = 0;
  const int rc = mbedtls_base64_decode(decodeBuf, sizeof(decodeBuf), &decodedLen,
                                       reinterpret_cast<const unsigned char*>(data), strlen(data));
  if (rc != 0 || decodedLen == 0) {
    sendError(otaId, "chunk", "base64_decode_failed");
    return;
  }
  if (rx.written + decodedLen > rx.expectedSize) {
    sendError(otaId, "chunk", "image_overflow");
    return;
  }
  if (cacheFile.write(decodeBuf, decodedLen) != decodedLen) {
    sendError(otaId, "chunk", "sd_write_failed");
    resetRx(true);
    return;
  }
  md5Builder.add(decodeBuf, decodedLen);

  rx.written += decodedLen;
  rx.nextSeq++;
  sendAck(otaId, "chunk", seq, offset, rx.written);
}

void handleEnd(JsonObjectConst obj) {
  const char* otaId = obj["otaId"] | "";
  if (!rx.active || strcmp(rx.otaId, otaId) != 0) {
    sendError(otaId, "end", "no_active_update");
    return;
  }
  if (rx.written != rx.expectedSize) {
    sendError(otaId, "end", "size_mismatch");
    resetRx(true);
    return;
  }
  cacheFile.close();

  // Whole-image MD5 (the daemon's transfer checksum)…
  if (strlen(rx.md5) == 32) {
    md5Builder.calculate();
    char calc[36] = {0};
    md5Builder.getChars(calc);
    if (strcasecmp(calc, rx.md5) != 0) {
      AgentLog::line("OTA", "md5 mismatch: got %s want %s", calc, rx.md5);
      sendError(otaId, "end", "md5_mismatch");
      resetRx(true);
      return;
    }
  }

  // …then the bootloader-mirror structural validation (magic, segments, XOR
  // checksum, SHA256 trailer). A few seconds — well inside the daemon's 30s
  // end-ack budget. The actual flash (~1 min of raw partition writes) must NOT
  // run here: ack first so the daemon reports success, then serviceFlash()
  // flashes from the main loop and restarts. A flash failure leaves the current
  // firmware bootable (otadata switches only after a complete write) and is
  // reported via esp32_ota_error + the unchanged buildHash after reconnect.
  const esp_partition_t* dest = esp_ota_get_next_update_partition(nullptr);
  const firmware_flash::Result vr = firmware_flash::validateImageFile(kCachePath, dest ? dest->size : 0);
  if (vr != firmware_flash::Result::OK) {
    sendError(otaId, "end", firmware_flash::resultName(vr));
    resetRx(true);
    return;
  }

  AgentLog::line("OTA", "image received+validated (%u bytes) — flash pending", (unsigned)rx.written);
  sendAck(otaId, "end", UINT32_MAX, rx.written, rx.written);
  rx.active = false;
  rx.flashPending = true;
}

void handleAbort(JsonObjectConst obj) {
  const char* otaId = obj["otaId"] | "";
  if (rx.active && (!otaId[0] || strcmp(rx.otaId, otaId) == 0)) {
    AgentLog::line("OTA", "aborted by daemon (id=%s)", otaId);
    resetRx(true);
  }
  sendAck(otaId, "abort", UINT32_MAX, 0, 0);
}

}  // namespace

bool maybeHandleFrame(const char* json, size_t length) {
  if (!json || length == 0 || length > kMaxOtaFrameBytes) return false;
  // Cheap probe before any JSON work. The daemon serializes {"type":"esp32_ota_…
  // first, but don't rely on position — just require the literal somewhere and
  // confirm with the parsed type below (a chat/timeline frame quoting this
  // string is >8 KB away from ever reaching here in practice, and the type
  // check keeps even that case correct).
  if (!memmem(json, length, "\"type\":\"esp32_ota_", 18)) return false;

  JsonDocument doc;  // OTA frames are ≤ ~1.6 KB — elastic doc stays small
  if (deserializeJson(doc, json, length) != DeserializationError::Ok) return false;
  JsonObjectConst obj = doc.as<JsonObjectConst>();
  const char* type = obj["type"] | "";
  if (strncmp(type, "esp32_ota_", 10) != 0) return false;

  if (strcmp(type, "esp32_ota_begin") == 0)
    handleBegin(obj);
  else if (strcmp(type, "esp32_ota_chunk") == 0)
    handleChunk(obj);
  else if (strcmp(type, "esp32_ota_end") == 0)
    handleEnd(obj);
  else if (strcmp(type, "esp32_ota_abort") == 0)
    handleAbort(obj);
  else
    return false;  // unknown esp32_ota_* — let the normal parser ignore it
  return true;
}

bool receiving() { return rx.active; }
bool flashPending() { return rx.flashPending; }
uint32_t receivedBytes() { return rx.written; }
uint32_t totalBytes() { return rx.expectedSize; }

void service() {
  if (!rx.active || rx.flashPending) return;
  if (millis() - rx.lastRxMs < kRxStallTimeoutMs) return;
  AgentLog::line("OTA", "receive stalled %us at %u/%u bytes — aborting", (unsigned)(kRxStallTimeoutMs / 1000),
                 (unsigned)rx.written, (unsigned)rx.expectedSize);
  sendError(rx.otaId, "chunk", "rx_stall_timeout");  // best-effort; the WS is likely gone
  resetRx(true);
}

void serviceFlash() {
  if (!rx.flashPending) return;
  rx.flashPending = false;

  AgentLog::line("OTA", "flashing %s", kCachePath);
  const firmware_flash::Result r =
      firmware_flash::flashFromSdPath(kCachePath, nullptr, nullptr, /*alreadyValidated=*/true);
  if (r == firmware_flash::Result::OK) {
    AgentLog::line("OTA", "flash OK — restarting");
    delay(250);
    ESP.restart();
    return;  // not reached
  }
  sendError(rx.otaId[0] ? rx.otaId : nullptr, "flash", firmware_flash::resultName(r));
  resetRx(true);
}

}  // namespace OtaWs
}  // namespace AgentDeck
