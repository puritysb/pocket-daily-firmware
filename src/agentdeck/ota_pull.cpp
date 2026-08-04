// HttpDownloader.h must precede anything that pulls lwip (WiFi/ArduinoJson via
// Arduino.h) — its SdFat macros collide with lwip's ip4_addr.h otherwise. See
// the same note at the top of src/network/OtaUpdater.cpp.
#include "ota_pull.h"

#include <HalStorage.h>
#include <esp_ota_ops.h>
#include <esp_wifi.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "agent/AgentLog.h"
#include "network/HttpDownloader.h"
#include "ota_ws_receiver.h"

namespace AgentDeck {
namespace OtaPull {
namespace {

// MD5 of the last image this device *committed to flashing*. Written BEFORE
// the flash on purpose: a structurally-valid image that still fails to flash
// must not be re-downloaded on every pull forever (5+ MB per 15 min would
// bleed the battery dry). The cost is that such an image needs a re-stage
// (new md5) to retry — the honest trade for an unattended device.
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

// Attempts within one wake. Each is a fresh HTTP GET resuming at the current
// offset; an attempt that transfers nothing at all ends the wake early.
constexpr int kMaxAttemptsPerWake = 6;

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

// Bytes already downloaded for this md5, or 0 when the marker is missing,
// stale, or disagrees with the cache file on disk (the file is the truth).
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
  return onDisk == (uint32_t)savedOffset ? onDisk : 0;
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

  for (int attempt = 0; attempt < kMaxAttemptsPerWake && have < fwSize; attempt++) {
    if (millis() - startedMs > kWakeBudgetMs) {
      AgentLog::line("OTA", "pull update: wake budget spent at %u/%u — resuming next wake", (unsigned)have,
                     (unsigned)fwSize);
      break;
    }

    char url[256];
    size_t o = (size_t)snprintf(url, sizeof(url), "http://%s:%u/esp32/fw?board=%s", ip, (unsigned)port, board);
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
    HttpDownloader::fetchUrl(url, [&out, &written, fwSize](const uint8_t* data, size_t len) {
      if (written + len > fwSize) len = fwSize - written;  // never overrun the advert
      if (len == 0) return false;
      if (out.write(data, len) != len) return false;
      written += (uint32_t)len;
      return true;
    });
    out.close();
    have = written;
    saveProgress(fwMd5, have);

    if (have >= fwSize) {
      complete = true;
      break;
    }
    if (have == before) {
      // Nothing moved at all: the endpoint is unreachable or refusing the
      // range. Retrying inside this wake would just spin the radio.
      AgentLog::line("OTA", "pull update: no progress at %u/%u — retrying next wake", (unsigned)have, (unsigned)fwSize);
      break;
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
