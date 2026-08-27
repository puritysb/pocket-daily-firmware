// HttpDownloader.h must precede anything that pulls lwip (WiFi/ArduinoJson via
// Arduino.h) — its SdFat macros collide with lwip's ip4_addr.h otherwise. See
// the same note at the top of src/network/OtaUpdater.cpp.
#include "ota_pull.h"

#include <HalStorage.h>
#include <esp_ota_ops.h>
#include <esp_wifi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "agent/AgentLog.h"
#include "network/HttpDownloader.h"
#include "ota_ws_receiver.h"
#include "pocket_daily/surface_request.h"

namespace AgentDeck {
namespace OtaPull {
namespace {

// MD5 of the last image this device committed to flashing. It is written
// before the flash so a restart cannot redownload a successfully-installed
// image. serviceFlash clears it on a returned failure; otherwise one failed
// flash permanently suppresses every retry of the still-current stage.
constexpr const char* kAppliedPath = "/.crosspoint/agentdeck-fw-applied.txt";

// Resume marker: "<md5> <bytesOnDisk>" for the partially-downloaded image in
// the shared OTA cache. See the resume rationale on tryInstall.
constexpr const char* kProgressPath = "/.crosspoint/agentdeck-fw-progress.txt";

// Below this battery level an unattended flash is a brick risk — skip and let
// a later (charged or docked) pull take it. -1 (unknown) passes: USB-powered
// boards without a gauge must not be locked out.
constexpr int kMinBatteryPct = 30;

// Per-wake download budget. A wake that can't finish persists its progress and
// lets the next cadence wake continue, so a link that only manages a few
// hundred KB per visit still converges instead of burning the battery in one
// long doomed attempt.
constexpr uint32_t kWakeBudgetMs = 150000;

// Attempts within one bounded pass. Each is a fresh HTTP GET resuming at the
// current offset; a zero-byte attempt reconnects within this limit, then the
// activity-level scheduler applies backoff and starts another pass.
constexpr int kMaxAttemptsPerWake = 6;

// Match the bridge's bounded response. The former 64 KiB budget aborted every
// 128 KiB response halfway through, throwing away the second half and doubling
// TCP/HTTP/redirect setup. 128 KiB is still a short, cooperative LAN burst.
constexpr uint32_t kInteractiveChunkBytes = 128 * 1024;

struct InteractiveTransfer {
  bool active = false;
  bool wifiPowerSaveSuspended = false;
  char ip[16] = {0};
  uint16_t port = 0;
  char token[40] = {0};
  char board[16] = {0};
  uint32_t fwSize = 0;
  char fwMd5[33] = {0};
};

InteractiveTransfer interactive;

void restoreInteractiveWifiPowerSave() {
  if (!interactive.wifiPowerSaveSuspended) return;
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  interactive.wifiPowerSaveSuspended = false;
}

void adoptRedirectHost(const HttpDownloader::EffectiveUrlCapture& effectiveUrl, char* ip, size_t ipCap) {
  if (!ip || ipCap == 0 || strncmp(effectiveUrl.value, "http://", 7) != 0) return;
  char redirectedIp[16] = {0};
  if (sscanf(effectiveUrl.value + 7, "%15[0-9.]", redirectedIp) != 1 || !redirectedIp[0]) return;
  if (strncmp(ip, redirectedIp, ipCap) == 0) return;
  AgentLog::line("OTA", "using redirected download path %s", redirectedIp);
  snprintf(ip, ipCap, "%s", redirectedIp);
}

bool readApplied(char* out, size_t cap) {
  out[0] = '\0';
  if (!Storage.exists(kAppliedPath)) return false;
  HalFile f = Storage.open(kAppliedPath, O_RDONLY);
  if (!f) return false;
  const int got = f.read(out, cap - 1);
  if (got <= 0) return false;
  out[got] = '\0';
  return true;
}

bool writeApplied(const char* md5) {
  HalFile f = Storage.open(kAppliedPath, O_WRITE | O_CREAT | O_TRUNC);
  if (!f) return false;
  const size_t len = strlen(md5);
  return f.write(md5, len) == len;
}

// Bytes already downloaded for this md5, or 0 when the marker/cache is missing
// or belongs to a different image. The cache file is the durable truth: power
// or Wi-Fi can disappear after an SD write but before saveProgress(), leaving
// it ahead of the marker. Conversely a torn/truncated file can be behind it.
// In both cases every byte physically present is still a prefix of this MD5-
// bound transfer; final whole-image validation remains the safety authority.
uint32_t loadProgress(const char* md5) {
  if (!Storage.exists(kProgressPath) || !Storage.exists(OtaWs::imageCachePath())) return 0;
  char buf[64] = {0};
  {
    HalFile f = Storage.open(kProgressPath, O_RDONLY);
    if (!f) return 0;
    const int got = f.read(buf, sizeof(buf) - 1);
    if (got <= 0) return 0;
    buf[got] = '\0';
  }
  char savedMd5[36] = {0};
  unsigned long savedOffset = 0;
  if (sscanf(buf, "%35s %lu", savedMd5, &savedOffset) != 2) return 0;
  if (strcasecmp(savedMd5, md5) != 0) return 0;  // a different build was staged since
  uint32_t onDisk = 0;
  {
    HalFile f;
    if (!Storage.openFileForRead("OTA", OtaWs::imageCachePath(), f)) return 0;
    onDisk = (uint32_t)f.size();
  }
  if (onDisk != (uint32_t)savedOffset) {
    AgentLog::line("OTA", "pull update: recovering cache/marker drift file=%u marker=%u", (unsigned)onDisk,
                   (unsigned)savedOffset);
  }
  return onDisk;
}

void saveProgress(const char* md5, uint32_t offset) {
  HalFile f = Storage.open(kProgressPath, O_WRITE | O_CREAT | O_TRUNC);
  if (!f) return;
  char buf[64];
  const int n = snprintf(buf, sizeof(buf), "%s %lu", md5, (unsigned long)offset);
  if (n > 0) f.write(buf, (size_t)n);
}

void clearProgress() { Storage.remove(kProgressPath); }

}  // namespace

void clearAppliedMarker() { Storage.remove(kAppliedPath); }

uint32_t savedBytes(const char* fwMd5) {
  if (!fwMd5 || strlen(fwMd5) != 32) return 0;
  return loadProgress(fwMd5);
}

bool alreadyApplied(const char* fwMd5) {
  if (!fwMd5 || strlen(fwMd5) != 32) return false;
  char applied[36] = {0};
  return readApplied(applied, sizeof(applied)) && strcasecmp(applied, fwMd5) == 0;
}

bool beginInteractive(const char* ip, uint16_t port, const char* token, const char* board, uint32_t fwSize,
                      const char* fwMd5, int battPct) {
  if (!fwSize || !fwMd5 || strlen(fwMd5) != 32 || !ip || !ip[0] || !port || !board || !board[0]) return false;
  if (OtaWs::receiving() || OtaWs::flashPending()) return false;
  if (battPct >= 0 && battPct < kMinBatteryPct) {
    AgentLog::line("OTA", "interactive update deferred: battery %d%% < %d%%", battPct, kMinBatteryPct);
    return false;
  }
  if (alreadyApplied(fwMd5)) return false;

  const esp_partition_t* dest = esp_ota_get_next_update_partition(nullptr);
  if (!dest || fwSize > dest->size) {
    AgentLog::line("OTA", "interactive update rejected: %u bytes vs slot %u", (unsigned)fwSize,
                   (unsigned)(dest ? dest->size : 0));
    return false;
  }

  interactive = {};
  interactive.active = true;
  snprintf(interactive.ip, sizeof(interactive.ip), "%s", ip);
  interactive.port = port;
  snprintf(interactive.token, sizeof(interactive.token), "%s", token ? token : "");
  snprintf(interactive.board, sizeof(interactive.board), "%s", board);
  interactive.fwSize = fwSize;
  snprintf(interactive.fwMd5, sizeof(interactive.fwMd5), "%s", fwMd5);

  // Keep modem sleep off for the complete foreground transfer. Toggling it for
  // every short request adds beacon latency to every reconnect and made the
  // next segment substantially more fragile on X3.
  esp_wifi_set_ps(WIFI_PS_NONE);
  interactive.wifiPowerSaveSuspended = true;

  uint32_t have = loadProgress(fwMd5);
  if (have > fwSize) have = 0;
  if (have == 0) {
    Storage.remove(OtaWs::imageCachePath());
    saveProgress(fwMd5, 0);
  }
  AgentLog::line("OTA", "interactive transfer ready at %u/%u", (unsigned)have, (unsigned)fwSize);
  return true;
}

InteractiveStep serviceInteractive() {
  if (!interactive.active) return InteractiveStep::Idle;
  if (OtaWs::receiving() || OtaWs::flashPending()) return InteractiveStep::Deferred;

  uint32_t have = loadProgress(interactive.fwMd5);
  if (have > interactive.fwSize) {
    have = 0;
    Storage.remove(OtaWs::imageCachePath());
    saveProgress(interactive.fwMd5, 0);
  }
  const uint32_t target = std::min<uint32_t>(interactive.fwSize, have + kInteractiveChunkBytes);

  char url[320];
  size_t o = (size_t)snprintf(url, sizeof(url), "http://%s:%u/esp32/fw?board=%s&productId=%s&updateChannel=%s",
                              interactive.ip, (unsigned)interactive.port, interactive.board, PocketDaily::PRODUCT_ID,
                              PocketDaily::UPDATE_CHANNEL);
  if (interactive.token[0]) o += (size_t)snprintf(url + o, sizeof(url) - o, "&token=%s", interactive.token);
  snprintf(url + o, sizeof(url) - o, "&from=%lu&limit=%lu", (unsigned long)have, (unsigned long)kInteractiveChunkBytes);

  HalFile out = Storage.open(OtaWs::imageCachePath(), O_RDWR | O_CREAT);
  if (!out || !out.seekSet(have)) {
    if (out) out.close();
    AgentLog::line("OTA", "interactive cache open/seek failed at %u", (unsigned)have);
    return InteractiveStep::Failed;
  }

  uint32_t written = have;
  const PocketDaily::SurfaceRequestHeaders surfaceIdentity(interactive.board);
  const auto requestHeaders = surfaceIdentity.view();
  HttpDownloader::EffectiveUrlCapture effectiveUrl;
  const uint32_t startedMs = millis();
  HttpDownloader::fetchUrl(
      url,
      [&out, &written, target](const uint8_t* data, size_t len) {
        const uint32_t room = target > written ? target - written : 0;
        const size_t accepted = std::min<size_t>(len, room);
        if (accepted && out.write(data, accepted) != accepted) return false;
        written += (uint32_t)accepted;
        // A Surface v1 bridge honors `limit`, so reaching target on the final
        // buffer is success. Abort only if an older/non-conforming endpoint
        // sends bytes beyond the cooperative window.
        return accepted == len;
      },
      "", "", &requestHeaders, &effectiveUrl);
  adoptRedirectHost(effectiveUrl, interactive.ip, sizeof(interactive.ip));
  out.close();
  saveProgress(interactive.fwMd5, written);

  if (written >= interactive.fwSize) {
    const uint32_t size = interactive.fwSize;
    char md5[33];
    snprintf(md5, sizeof(md5), "%s", interactive.fwMd5);
    interactive.active = false;
    restoreInteractiveWifiPowerSave();
    if (!OtaWs::stagePulledImage(size, md5)) {
      clearProgress();
      return InteractiveStep::Failed;
    }
    if (!writeApplied(md5)) AgentLog::line("OTA", "interactive applied marker write failed");
    clearProgress();
    AgentLog::line("OTA", "interactive image staged — flash handoff pending");
    return InteractiveStep::Staged;
  }

  if (written > have) {
    const uint32_t elapsedMs = std::max<uint32_t>(1, millis() - startedMs);
    const uint32_t bytes = written - have;
    AgentLog::line("OTA", "interactive chunk %u/%u: +%u in %ums (%u KiB/s)", (unsigned)written,
                   (unsigned)interactive.fwSize, (unsigned)bytes, (unsigned)elapsedMs,
                   (unsigned)((bytes * 1000ULL / elapsedMs) / 1024ULL));
    return InteractiveStep::Progress;
  }
  return InteractiveStep::Retry;
}

bool interactiveActive() { return interactive.active; }

void cancelInteractive() {
  interactive.active = false;
  restoreInteractiveWifiPowerSave();
}

bool tryInstall(const char* ip, uint16_t port, const char* token, const char* board, uint32_t fwSize, const char* fwMd5,
                int battPct) {
  if (!fwSize || !fwMd5 || strlen(fwMd5) != 32 || !ip || !ip[0] || !port || !board) return false;
  if (OtaWs::receiving() || OtaWs::flashPending()) return false;
  if (battPct >= 0 && battPct < kMinBatteryPct) {
    AgentLog::line("OTA", "pull update deferred: battery %d%% < %d%%", battPct, kMinBatteryPct);
    return false;
  }

  char applied[36] = {0};
  if (readApplied(applied, sizeof(applied)) && strcasecmp(applied, fwMd5) == 0) return false;  // already taken

  const esp_partition_t* dest = esp_ota_get_next_update_partition(nullptr);
  if (!dest || fwSize > dest->size) {
    AgentLog::line("OTA", "pull update rejected: %u bytes vs slot %u", (unsigned)fwSize,
                   (unsigned)(dest ? dest->size : 0));
    return false;
  }

  // ── Resumable segmented download ──
  // A 5 MB image is far too large to assume one healthy TCP connection: on a
  // marginal link (or a bridge host with two NICs on one subnet, where full-MTU
  // segments in one direction get black-holed) a single GET dies partway every
  // time, and restarting from zero never converges. So: ask for `?from=<have>`,
  // append whatever arrives, and re-ask. Every attempt is short and any progress
  // is kept — including across sleeps, via the marker file — which turns an
  // unreliable link from "never updates" into "updates a bit slower".
  uint32_t have = loadProgress(fwMd5);
  if (have > fwSize) have = 0;  // marker/cache disagree with the advert — restart
  if (have == 0) Storage.remove(OtaWs::imageCachePath());
  if (have)
    AgentLog::line("OTA", "pull update resuming at %u/%u bytes", (unsigned)have, (unsigned)fwSize);
  else
    AgentLog::line("OTA", "pull update: fetching %u bytes (md5 %s)", (unsigned)fwSize, fwMd5);

  // Modem power-save adds beacon-interval latency to every TCP segment of a
  // multi-MB body; suspend it for the download exactly like the WS receive path.
  esp_wifi_set_ps(WIFI_PS_NONE);
  const uint32_t startedMs = millis();
  bool complete = false;
  char downloadIp[16] = {0};
  snprintf(downloadIp, sizeof(downloadIp), "%s", ip);

  for (int attempt = 0; attempt < kMaxAttemptsPerWake && have < fwSize; attempt++) {
    if (millis() - startedMs > kWakeBudgetMs) {
      AgentLog::line("OTA", "pull update: wake budget spent at %u/%u — resuming next wake", (unsigned)have,
                     (unsigned)fwSize);
      break;
    }

    char url[320];
    size_t o =
        (size_t)snprintf(url, sizeof(url), "http://%s:%u/esp32/fw?board=%s&productId=%s&updateChannel=%s", downloadIp,
                         (unsigned)port, board, PocketDaily::PRODUCT_ID, PocketDaily::UPDATE_CHANNEL);
    if (token && token[0]) o += (size_t)snprintf(url + o, sizeof(url) - o, "&token=%s", token);
    snprintf(url + o, sizeof(url) - o, "&from=%lu", (unsigned long)have);

    // NOT openFileForWrite: that opens O_RDWR|O_CREAT|O_TRUNC (SDCardManager.cpp
    // :228), which would empty the partial image on every resumed attempt and
    // leave the bytes before `have` undefined. Open without O_TRUNC and seek.
    HalFile out = Storage.open(OtaWs::imageCachePath(), O_RDWR | O_CREAT);
    if (!out) {
      AgentLog::line("OTA", "pull update: cache open failed");
      break;
    }
    if (!out.seekSet(have)) {
      AgentLog::line("OTA", "pull update: cache seek to %u failed", (unsigned)have);
      out.close();
      break;
    }
    const uint32_t before = have;
    uint32_t written = have;
    const PocketDaily::SurfaceRequestHeaders surfaceIdentity(board);
    const auto requestHeaders = surfaceIdentity.view();
    HttpDownloader::EffectiveUrlCapture effectiveUrl;
    HttpDownloader::fetchUrl(
        url,
        [&out, &written, fwSize](const uint8_t* data, size_t len) {
          if (written + len > fwSize) len = fwSize - written;  // never overrun the advert
          if (len == 0) return false;
          if (out.write(data, len) != len) return false;
          written += (uint32_t)len;
          return true;
        },
        "", "", &requestHeaders, &effectiveUrl);
    adoptRedirectHost(effectiveUrl, downloadIp, sizeof(downloadIp));
    out.close();
    have = written;
    saveProgress(fwMd5, have);

    if (have >= fwSize) {
      complete = true;
      break;
    }
    if (have == before) {
      // One socket can be admitted while its data path is black-holed on a
      // dual-NIC same-subnet host. Treat that as a transient connection, not a
      // reason to make the user press Sync again. The loop's attempt count and
      // wake budget remain the hard bounds, so a genuinely absent endpoint
      // still returns to offline UI instead of spinning indefinitely.
      AgentLog::line("OTA", "pull update: no progress at %u/%u — reconnecting", (unsigned)have, (unsigned)fwSize);
      delay(250);
      continue;
    }
    AgentLog::line("OTA", "pull update: %u/%u bytes", (unsigned)have, (unsigned)fwSize);
  }

  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  if (!complete) return false;  // progress persisted; the next pull continues

  // Validate first. Marking the advert as applied before this point would make
  // a transiently-corrupt download (MD5/structure mismatch) permanently skip
  // every retry of the same, still-valid staged build.
  if (!OtaWs::stagePulledImage(fwSize, fwMd5)) {
    clearProgress();  // restart cleanly on the next advert pull
    return false;
  }

  // The image is now structurally valid and committed to the flash path. Keep
  // the marker ahead of serviceFlash() so a genuine flash failure does not
  // download the same 5+ MB image on every cadence wake.
  if (!writeApplied(fwMd5)) AgentLog::line("OTA", "pull update: applied marker write failed");
  clearProgress();
  AgentLog::line("OTA", "pull update staged — flashing on this wake");
  return true;
}

}  // namespace OtaPull
}  // namespace AgentDeck
