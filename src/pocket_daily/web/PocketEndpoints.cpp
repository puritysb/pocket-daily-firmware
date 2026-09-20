#include "pocket_daily/web/PocketEndpoints.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <Logging.h>
#include <WebServer.h>
#include <esp_ota_ops.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "network/FirmwareFlasher.h"
#include "pocket_daily/PocketScreenPreview.h"
#include "pocket_daily/live_studio/DevTrace.h"
#include "pocket_daily/live_studio/HeapMap.h"
#include "pocket_daily/live_studio/LiveFrameCapture.h"
#include "pocket_daily/live_studio/NetHealth.h"
#include "pocket_daily/live_studio/StackReport.h"
#include "pocket_daily/live_studio/UiPackStore.h"
#include "pocket_daily/web/PocketStatus.h"
#include "util/BookCacheUtils.h"
#ifdef ENABLE_DEV_REMOTE_FLASH
#include "pocket_daily/product_identity.h"
#endif

namespace PocketDaily::Web {
namespace {

// Keep diagnostic responses below the Pocket private AP's scarce contiguous
// heap and return to the global loop between every piece. The previous 512 B
// write loop could block inside lwIP long enough to trip the task watchdog.
// Each diagnostic request reads one chunk into the shared static staging
// buffer and answers with one bounded socket write. A 53 KB X3 frame needs
// ~13 requests instead of ~52.
constexpr size_t CRASH_REPORT_CHUNK_BYTES = 1024;
constexpr size_t SCREEN_PREVIEW_CHUNK_BYTES = firmware_flash::STAGING_BUFFER_BYTES;
// A diagnostic chunk send blocks the loop task. Unlike an SD write it can
// hang forever if the peer vanishes, so the loop watchdog must stay armed;
// bounding the socket below the 5 s task-WDT window guarantees the send
// returns (completed or aborted) before the watchdog could fire.
constexpr unsigned long DIAGNOSTIC_SEND_TIMEOUT_MS = 3000;

// LS-2 live frame fetch: same chunked octet-stream contract as the one-shot
// screen preview, but reading the latest captured frame. Single slot — the
// companion associates the fetched bytes with the newest `frame` event.
// The fetch gate sits below the diagnostics floor on purpose: a 4 KiB shared
// -buffer read plus one bounded send is far lighter than the crash report,
// and an X3 in STA File Transfer idles within a few hundred bytes of the
// 10 KiB diagnostics floor, which starved multi-chunk fetches in testing.
constexpr uint32_t LIVE_FETCH_MIN_FREE_HEAP = 6U * 1024U;

void note(const RouteDeps& d) {
  if (d.host.noteClientActivity) d.host.noteClientActivity(d.host.self);
}

void repaint(const RouteDeps& d) {
  if (d.host.requestRepaint) d.host.requestRepaint(d.host.self);
}

String norm(const RouteDeps& d, const String& path) {
  return d.host.normalizeWebPath ? d.host.normalizeWebPath(d.host.self, path) : path;
}

bool protectedName(const RouteDeps& d, const String& name) {
  return d.host.isProtectedItemName && d.host.isProtectedItemName(d.host.self, name);
}

bool renameStorageFile(const String& from, const String& to) {
  HalFile file = Storage.open(from.c_str());
  if (!file || file.isDirectory()) {
    if (file) file.close();
    return false;
  }
  const bool renamed = file.rename(to.c_str());
  file.close();
  return renamed;
}

void handleSessionEnd(WebServer& server, const RouteDeps& d) {
  if ((d.host.httpUploadBusy && d.host.httpUploadBusy(d.host.self)) || (d.stream && d.stream->transferActive())) {
    server.send(409, "application/json", "{\"error\":\"transfer active\"}");
    return;
  }
  server.send(200, "application/json", "{\"ended\":true}");
  d.host.endDirectSession(d.host.self);
}

void handleCrashReport(WebServer& server) {
  HalSystem::setCrashBreadcrumb("nearby:crash-chunk");
  if (!diagnosticsAffordable()) {
    server.send(503, "text/plain", "Reader memory is too low for diagnostics right now");
    return;
  }
  HalFile report = Storage.open("/crash_report.txt");
  if (!report || report.isDirectory()) {
    if (report) report.close();
    server.send(404, "text/plain", "No crash report recorded");
    return;
  }

  if (!server.hasArg("offset")) {
    report.close();
    server.send(400, "text/plain", "Missing crash report offset");
    return;
  }

  const String offsetText = server.arg("offset");
  if (offsetText.isEmpty()) {
    report.close();
    server.send(400, "text/plain", "Invalid crash report offset");
    return;
  }
  for (size_t i = 0; i < offsetText.length(); i++) {
    if (offsetText[i] < '0' || offsetText[i] > '9') {
      report.close();
      server.send(400, "text/plain", "Invalid crash report offset");
      return;
    }
  }

  const size_t reportSize = report.size();
  const size_t offset = static_cast<size_t>(offsetText.toInt());
  if (offset >= reportSize || !report.seek(offset)) {
    report.close();
    server.send(416, "text/plain", "Crash report offset out of range");
    return;
  }

  uint8_t* body = firmware_flash::sharedStagingBuffer();
  const size_t remaining = reportSize - offset;
  const size_t requested = std::min(remaining, CRASH_REPORT_CHUNK_BYTES);
  const int count = report.read(body, requested);
  report.close();
  if (count <= 0) {
    server.send(500, "text/plain", "Could not read crash report chunk");
    return;
  }

  server.client().setTimeout(DIAGNOSTIC_SEND_TIMEOUT_MS);
  feedLoopWDT();
  server.setContentLength(static_cast<size_t>(count));
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain; charset=utf-8", "");
  server.sendContent(reinterpret_cast<const char*>(body), static_cast<size_t>(count));
  feedLoopWDT();
  LOG_DBG("WEB", "Crash report chunk offset=%u bytes=%u total=%u", static_cast<unsigned>(offset),
          static_cast<unsigned>(count), static_cast<unsigned>(reportSize));
}

// Pocket clients upload to a unique hidden .part file, then ask the reader
// to verify size + CRC and atomically publish it. A dropped phone or Wi-Fi
// link therefore never turns a valid book or update.bin into a partial file.
void handleCommitUpload(WebServer& server, const RouteDeps& d) {
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "Missing JSON body");
    return;
  }

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, server.arg("plain"));
  if (error || !doc["staging"].is<const char*>() || !doc["target"].is<const char*>() || !doc["size"].is<size_t>() ||
      !doc["crc32"].is<const char*>()) {
    server.send(400, "text/plain", "Invalid commit request");
    return;
  }

  const String staging = norm(d, doc["staging"].as<const char*>());
  const String target = norm(d, doc["target"].as<const char*>());
  const size_t expectedSize = doc["size"].as<size_t>();
  const String expectedCrcText = doc["crc32"].as<const char*>();
  char* crcEnd = nullptr;
  const uint32_t expectedCrc = strtoul(expectedCrcText.c_str(), &crcEnd, 16);

  const int stagingSlash = staging.lastIndexOf('/');
  const int targetSlash = target.lastIndexOf('/');
  const String stagingName = staging.substring(stagingSlash + 1);
  const String targetName = target.substring(targetSlash + 1);
  const String stagingParent = staging.substring(0, stagingSlash + 1);
  const String targetParent = target.substring(0, targetSlash + 1);
  if (!stagingName.startsWith(".pocket-") || !stagingName.endsWith(".part") || stagingParent != targetParent ||
      targetName.isEmpty() || protectedName(d, targetName) || !crcEnd || *crcEnd != '\0' ||
      expectedCrcText.length() != 8) {
    server.send(400, "text/plain", "Unsafe commit path or checksum");
    return;
  }

  const StagedUpload& staged = d.stream->staged();
  String uploadedPath = norm(d, staged.path + "/" + staged.fileName);
  const uint32_t uploadedCrc = staged.crc32 ^ 0xFFFFFFFFU;
  if (!staged.success || uploadedPath != staging || staged.size != expectedSize || uploadedCrc != expectedCrc ||
      !Storage.exists(staging.c_str())) {
    server.send(409, "text/plain", "Staged upload verification failed");
    return;
  }

  HalFile stagedFile = Storage.open(staging.c_str());
  const bool stagedSizeMatches = stagedFile && !stagedFile.isDirectory() && stagedFile.size() == expectedSize;
  if (stagedFile) stagedFile.close();
  if (!stagedSizeMatches) {
    server.send(409, "text/plain", "Staged file size mismatch");
    return;
  }

  const String backup = stagingParent + ".pocket-backup.part";
  Storage.remove(backup.c_str());
  const bool hadTarget = Storage.exists(target.c_str());
  if (hadTarget && !renameStorageFile(target, backup)) {
    server.send(500, "text/plain", "Could not preserve existing target");
    return;
  }
  if (!renameStorageFile(staging, target)) {
    if (hadTarget) renameStorageFile(backup, target);
    server.send(500, "text/plain", "Could not publish staged upload");
    return;
  }
  if (hadTarget) Storage.remove(backup.c_str());

  clearBookCache(target.c_str());
  char response[80];
  snprintf(response, sizeof(response), "{\"size\":%u,\"crc32\":\"%08lX\"}", static_cast<unsigned>(expectedSize),
           static_cast<unsigned long>(uploadedCrc));
  server.send(200, "application/json", response);
  LOG_INF("WEB", "Committed Pocket upload %s (%u bytes, crc32=%08lX)", target.c_str(),
          static_cast<unsigned>(expectedSize), static_cast<unsigned long>(uploadedCrc));
}

void handleScreenPreview(WebServer& server) {
  HalSystem::setCrashBreadcrumb("nearby:screen-preview");
  if (!diagnosticsAffordable()) {
    server.send(503, "text/plain", "Reader memory is too low for diagnostics right now");
    return;
  }
  HalFile preview = Storage.open(PocketDaily::SCREEN_PREVIEW_PATH);
  if (!preview || preview.isDirectory()) {
    if (preview) preview.close();
    server.send(404, "text/plain", "No Pocket Daily screen preview available");
    return;
  }

  if (!server.hasArg("offset")) {
    preview.close();
    server.send(400, "text/plain", "Missing screen preview offset");
    return;
  }

  const String offsetText = server.arg("offset");
  if (offsetText.isEmpty()) {
    preview.close();
    server.send(400, "text/plain", "Invalid screen preview offset");
    return;
  }
  for (size_t i = 0; i < offsetText.length(); i++) {
    if (offsetText[i] < '0' || offsetText[i] > '9') {
      preview.close();
      server.send(400, "text/plain", "Invalid screen preview offset");
      return;
    }
  }

  const size_t previewSize = preview.size();
  const size_t offset = static_cast<size_t>(offsetText.toInt());
  if (offset >= previewSize || !preview.seek(offset)) {
    preview.close();
    server.send(416, "text/plain", "Screen preview offset out of range");
    return;
  }

  uint8_t* body = firmware_flash::sharedStagingBuffer();
  const size_t requested = std::min(previewSize - offset, SCREEN_PREVIEW_CHUNK_BYTES);
  const int count = preview.read(body, requested);
  preview.close();
  if (count <= 0) {
    server.send(500, "text/plain", "Could not read screen preview chunk");
    return;
  }

  // The blocking TCP send can exceed the private-AP loop watchdog window on a
  // weak link (measured: a task-watchdog reset at nearby:screen-preview while
  // streaming 4 KiB chunks). Suspend the loop task's watchdog around the send,
  // exactly as the SD upload path does, and bound the peer wait so a vanished
  // companion cannot hang the reader while the watchdog is off.
  // Keep the watchdog armed; the bounded socket timeout, not a WDT suspension,
  // is what prevents a stuck send from either hanging the reader or tripping
  // the 5 s task watchdog.
  server.client().setTimeout(DIAGNOSTIC_SEND_TIMEOUT_MS);
  feedLoopWDT();
  server.setContentLength(static_cast<size_t>(count));
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/octet-stream", "");
  server.sendContent(reinterpret_cast<const char*>(body), static_cast<size_t>(count));
  feedLoopWDT();
  LOG_DBG("WEB", "Pocket screen preview offset=%u bytes=%u total=%u", static_cast<unsigned>(offset),
          static_cast<unsigned>(count), static_cast<unsigned>(previewSize));
}

// Pocket only needs four preferences. Avoid constructing and streaming the
// full localized settings registry on the no-PSRAM private-AP path: that
// response can consume the last contiguous heap immediately after Wi-Fi
// starts and strand the X3 on its retained Hotspot Mode frame.
void handleGetPreferences(WebServer& server) {
  char json[192];
  const int written = snprintf(
      json, sizeof(json), "{\"startupApp\":%u,\"pocketDailySleepCover\":%u,\"sleepTimeoutMinutes\":%u,\"fontSize\":%u}",
      static_cast<unsigned>(SETTINGS.startupApp), static_cast<unsigned>(SETTINGS.pocketDailySleepCover),
      static_cast<unsigned>(SETTINGS.sleepTimeoutMinutes), static_cast<unsigned>(SETTINGS.fontSize));
  if (written <= 0 || static_cast<size_t>(written) >= sizeof(json)) {
    server.send(500, "text/plain", "Could not encode Pocket preferences");
    return;
  }
  server.sendHeader("Connection", "close");
  server.send(200, "application/json", json);
}

void handlePostPreferences(WebServer& server, const RouteDeps& d) {
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "Missing JSON body");
    return;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    server.send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  if (!doc["startupApp"].isNull()) {
    const int value = doc["startupApp"].as<int>();
    if (value < 0 || value >= CrossPointSettings::STARTUP_APP_COUNT) {
      server.send(400, "text/plain", "Invalid startupApp");
      return;
    }
    SETTINGS.startupApp = static_cast<uint8_t>(value);
  }
  if (!doc["pocketDailySleepCover"].isNull()) {
    SETTINGS.pocketDailySleepCover = doc["pocketDailySleepCover"].as<int>() ? 1 : 0;
  }
  if (!doc["sleepTimeoutMinutes"].isNull()) {
    const int value = doc["sleepTimeoutMinutes"].as<int>();
    if (value < CrossPointSettings::MIN_SLEEP_TIMEOUT_MINUTES ||
        value > CrossPointSettings::MAX_SLEEP_TIMEOUT_MINUTES) {
      server.send(400, "text/plain", "Invalid sleepTimeoutMinutes");
      return;
    }
    SETTINGS.sleepTimeoutMinutes = static_cast<uint8_t>(value);
  }
  if (!doc["fontSize"].isNull()) {
    const int value = doc["fontSize"].as<int>();
    if (value < 0 || value >= CrossPointSettings::FONT_SIZE_COUNT) {
      server.send(400, "text/plain", "Invalid fontSize");
      return;
    }
    SETTINGS.fontSize = static_cast<uint8_t>(value);
  }

  if (!SETTINGS.saveToFile()) {
    server.send(500, "text/plain", "Could not save Pocket preferences");
    return;
  }
  server.sendHeader("Connection", "close");
  server.send(200, "application/json", "{\"saved\":true}");
  d.liveStudio->notifyPrefsChanged();
}

void handleScreenLive(WebServer& server) {
  HalSystem::setCrashBreadcrumb("nearby:screen-live");
  if (ESP.getFreeHeap() < LIVE_FETCH_MIN_FREE_HEAP) {
    server.send(503, "text/plain", "Reader memory is too low for the live frame right now");
    return;
  }
  HalFile frame = Storage.open(PocketDaily::LiveFrameCapture::LIVE_FRAME_PATH);
  if (!frame || frame.isDirectory()) {
    if (frame) frame.close();
    server.send(404, "text/plain", "No live frame captured yet");
    return;
  }
  if (!server.hasArg("offset")) {
    frame.close();
    server.send(400, "text/plain", "Missing live frame offset");
    return;
  }
  const String offsetText = server.arg("offset");
  bool offsetValid = !offsetText.isEmpty();
  for (size_t i = 0; offsetValid && i < offsetText.length(); i++) {
    if (offsetText[i] < '0' || offsetText[i] > '9') offsetValid = false;
  }
  if (!offsetValid) {
    frame.close();
    server.send(400, "text/plain", "Invalid live frame offset");
    return;
  }
  const size_t frameSize = frame.size();
  const size_t offset = static_cast<size_t>(offsetText.toInt());
  if (offset >= frameSize || !frame.seek(offset)) {
    frame.close();
    server.send(416, "text/plain", "Live frame offset out of range");
    return;
  }
  uint8_t* body = firmware_flash::sharedStagingBuffer();
  const size_t requested = std::min(frameSize - offset, SCREEN_PREVIEW_CHUNK_BYTES);
  const int count = frame.read(body, requested);
  frame.close();
  if (count <= 0) {
    server.send(500, "text/plain", "Could not read live frame chunk");
    return;
  }
  server.client().setTimeout(DIAGNOSTIC_SEND_TIMEOUT_MS);
  feedLoopWDT();
  server.setContentLength(static_cast<size_t>(count));
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/octet-stream", "");
  server.sendContent(reinterpret_cast<const char*>(body), static_cast<size_t>(count));
  feedLoopWDT();
}

void handleUiPackList(WebServer& server, const RouteDeps& d) {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  server.sendContent("[");
  char line[160];
  bool first = true;
  d.host.scanUiPacks(d.host.self, [&](const char* rawName, bool isDirectory, size_t size) {
    const size_t rawLen = strlen(rawName);
    if (isDirectory || rawLen < 7 || strcmp(rawName + rawLen - 7, ".uipack") != 0) return;
    // Strip the extension for the pack name the apply endpoint expects.
    const std::string name(rawName, rawLen - 7);
    const bool active = name == d.liveStudio->activePackName();
    const int n = snprintf(line, sizeof(line), R"({"name":"%.32s","size":%u,"active":%s})", name.c_str(),
                           static_cast<unsigned>(size), active ? "true" : "false");
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(line)) return;
    if (!first) server.sendContent(",");
    first = false;
    server.sendContent(line);
  });
  server.sendContent("]");
  server.sendContent("");
}

// LS-3 apply: load + validate + layer over the current theme, persist the
// choice, and ask for a repaint so the live frame shows the result. An empty
// name reverts to the theme's own metrics.
void handleUiPackApply(WebServer& server, const RouteDeps& d) {
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }
  const char* name = doc["name"] | "";
  if (name[0] == '\0') {
    PocketDaily::LiveStudio::ThemeOverride none{};
    UITheme::getInstance().applyPackMetrics(&none, 0);
    d.liveStudio->onPackCleared();
    repaint(d);
    server.send(200, "application/json", "{\"applied\":false}");
    return;
  }
  PocketDaily::LiveStudio::UiPackInfo info;
  PocketDaily::LiveStudio::ThemeOverride* overrides = PocketDaily::LiveStudio::acquireOverrideBuffer();
  if (!overrides) {
    server.send(503, "text/plain", "Not enough memory to load the pack");
    return;
  }
  PocketDaily::LiveStudio::UiPackResult validateError = PocketDaily::LiveStudio::UiPackResult::Ok;
  const auto result = PocketDaily::LiveStudio::loadPackFromSd(
      name, &info, overrides, PocketDaily::LiveStudio::UIPACK_MAX_THEME_OVERRIDES, &validateError);
  if (result != PocketDaily::LiveStudio::StoreResult::Ok) {
    PocketDaily::LiveStudio::releaseOverrideBuffer(overrides);
    const int code = result == PocketDaily::LiveStudio::StoreResult::OpenFail ? 404 : 422;
    char body[128];
    snprintf(body, sizeof(body), "{\"applied\":false,\"error\":\"%s\"}",
             PocketDaily::LiveStudio::storeResultName(result));
    server.send(code, "application/json", body);
    return;
  }
  UITheme::getInstance().applyPackMetrics(overrides, info.themeOverrideCount);
  PocketDaily::LiveStudio::releaseOverrideBuffer(overrides);
  d.liveStudio->onPackApplied(info.name, info.packVersion);
  repaint(d);
  char body[128];
  snprintf(body, sizeof(body), "{\"applied\":true,\"name\":\"%.32s\",\"version\":\"%.16s\",\"overrides\":%u}",
           info.name, info.packVersion, static_cast<unsigned>(info.themeOverrideCount));
  server.send(200, "application/json", body);
}

#ifdef ENABLE_DEV_REMOTE_FLASH
// Developer builds only. Validates and flashes the staged /update.bin, then
// reboots; a marker makes the next boot land in the File Transfer menu where
// one Confirm rejoins the saved network. The 200 response is sent before
// flashing because the loop blocks for the erase/write and ends in a chip
// restart.
void handleDevFlash(WebServer& server) {
  HalSystem::setCrashBreadcrumb("dev:remote-flash");
  // Allowed on the reader's own hotspot too: when the STA path is the thing
  // being repaired, the dedicated AP link is the delivery route (and it has
  // no router in the path). Still a dev-build-only endpoint.
  if (!Storage.exists("/update.bin")) {
    server.send(404, "text/plain", "No /update.bin staged on the reader");
    return;
  }
  const esp_partition_t* dest = esp_ota_get_next_update_partition(nullptr);
  if (!dest) {
    server.send(500, "text/plain", "No OTA partition available");
    return;
  }
  if (firmware_flash::validateImageFile("/update.bin", dest->size) != firmware_flash::Result::OK) {
    server.send(422, "text/plain", "Staged /update.bin failed image validation");
    return;
  }
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", "Flashing /update.bin (dev build). Reader reboots to File Transfer.");
  {
    HalFile marker;
    if (Storage.openFileForWrite("DEV", PocketDaily::DEV_BOOT_FILE_TRANSFER_MARKER, marker) && marker) {
      marker.write(reinterpret_cast<const uint8_t*>("1"), 1);
      marker.close();
    }
  }
  delay(750);  // Put the response on the wire before the loop disappears.
  if (firmware_flash::flashFromSdPath("/update.bin", nullptr, nullptr, true) != firmware_flash::Result::OK) {
    LOG_ERR("WEB", "Dev remote flash failed; staying up");
    Storage.remove(PocketDaily::DEV_BOOT_FILE_TRANSFER_MARKER);
    return;
  }
  ESP.restart();
}
#endif

}  // namespace

void registerPocketRoutes(WebServer& server, const RouteDeps& d) {
  if (d.profile == Profile::POCKET_SYNC && d.apMode) {
    server.on("/api/pocket/v1/session/end", HTTP_POST,
              [server = &server, deps = &d] { handleSessionEnd(*server, *deps); });
  }
  // Diagnostics is available in every profile so a phone can retrieve the
  // previous panic after the reader has recovered, without exposing arbitrary
  // hidden files or requiring the SD card to be removed.
  server.on("/api/pocket/v1/crash-report", HTTP_GET, [server = &server, deps = &d] {
    note(*deps);
    handleCrashReport(*server);
  });

  server.on("/api/pocket/v1/commit", HTTP_POST, [server = &server, deps = &d] {
    note(*deps);
    handleCommitUpload(*server, *deps);
  });

  if (d.profile == Profile::POCKET_SYNC) {
    server.on("/api/pocket/v1/screen-preview", HTTP_GET, [server = &server, deps = &d] {
      note(*deps);
      handleScreenPreview(*server);
    });
    server.on("/api/pocket/v1/preferences", HTTP_GET, [server = &server, deps = &d] {
      note(*deps);
      handleGetPreferences(*server);
    });
    server.on("/api/pocket/v1/preferences", HTTP_POST, [server = &server, deps = &d] {
      note(*deps);
      handlePostPreferences(*server, *deps);
    });
  } else {
    // Live Studio LS-2/LS-3: chunked fetch of the latest captured frame and
    // the UI-pack endpoints, served wherever the WS push listener can run
    // (STA profiles).
    server.on("/api/pocket/v1/screen-live", HTTP_GET, [server = &server, deps = &d] {
      note(*deps);
      handleScreenLive(*server);
    });
    server.on("/api/pocket/v1/ui-packs", HTTP_GET, [server = &server, deps = &d] {
      note(*deps);
      handleUiPackList(*server, *deps);
    });
    server.on("/api/pocket/v1/ui-pack/apply", HTTP_POST, [server = &server, deps = &d] {
      note(*deps);
      handleUiPackApply(*server, *deps);
    });
  }
#if POCKET_HEAP_MAP_ENABLED
  // HN-2 evidence part 2: per-task stack high-water marks. Safe API per task
  // (no scheduler suspension, unlike uxTaskGetSystemState which hung).
  server.on("/api/pocket/v1/dev/stack-report", HTTP_GET, [server = &server, deps = &d] {
    note(*deps);
    char report[640];
    const size_t n = PocketDaily::StackReport::render(report, sizeof(report));
    server->send(200, "text/plain; charset=utf-8", n ? String(report) : "unavailable");
  });
  // HN-2 evidence: caps-only, fixed small response. The fuller map hung the
  // request on the loaded X3 (uxTaskGetSystemState suspends every task; the
  // chunked variant never answered). Three numbers per capability survive
  // any fragmentation.
  server.on("/api/pocket/v1/dev/heap-map", HTTP_GET, [server = &server, deps = &d] {
    note(*deps);
    char map[256];
    const size_t n = PocketDaily::HeapMap::renderCaps(map, sizeof(map));
    server->send(200, "text/plain; charset=utf-8", n ? String(map) : "unavailable");
  });
#endif
#ifdef ENABLE_DEV_REMOTE_FLASH
  // Developer builds only: flash the staged /update.bin over the LAN so
  // iteration does not walk the on-device Settings menus. Absent from
  // gh_release builds by build flag, not by request filtering.
  server.on("/api/pocket/v1/dev/flash", HTTP_POST, [server = &server, deps = &d] {
    note(*deps);
    handleDevFlash(*server);
  });
  // Developer builds only: force one repaint (live-frame debugging).
  server.on("/api/pocket/v1/dev/render", HTTP_POST, [server = &server, deps = &d] {
    note(*deps);
    DEV_TRACE(PocketDaily::DevTrace::RENDER_REQ);
    repaint(*deps);
    server->send(204, "text/plain", "");
  });

  // Developer builds only: network stability post-mortem log
  // (docs/network-stability-analysis.md). Readable after the radio died.
  server.on("/api/pocket/v1/dev/net-health", HTTP_GET, [server = &server, deps = &d] {
    note(*deps);
    static char tail[4096];
    const size_t n = PocketDaily::NetHealth::tail(tail, sizeof(tail));
    server->send(200, "text/plain; charset=utf-8", n ? String(tail) : "no net-health log yet");
  });
  // Developer builds only: dump the live-studio trace ring (text lines
  // "ms tag aux"), oldest first. A gap between heartbeats is the stall.
  server.on("/api/pocket/v1/dev/live-debug", HTTP_GET, [server = &server, deps = &d] {
    note(*deps);
    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    server->send(200, "text/plain", "");
    static const char* names[] = {"HEARTBEAT",    "RENDER_START", "RENDER_DONE", "CAPTURE_ENTER", "CAPTURE_WRITE",
                                  "CAPTURE_DONE", "SEND_START",   "SEND_DONE",   "RENDER_REQ",    "TICK"};
    const size_t next = PocketDaily::DevTrace::gNext.load();
    char line[48];
    for (size_t k = 0; k < PocketDaily::DevTrace::CAPACITY; k++) {
      const auto& e = PocketDaily::DevTrace::gEntries[(next + k) % PocketDaily::DevTrace::CAPACITY];
      if (e.tag == 0 && e.ms == 0) continue;
      snprintf(line, sizeof(line), "%lu %s %u", static_cast<unsigned long>(e.ms),
               e.tag < sizeof(names) / sizeof(names[0]) ? names[e.tag] : "?", e.aux);
      server->sendContent(line);
      server->sendContent("\n");
    }
    server->sendContent("");
  });
#endif
}

}  // namespace PocketDaily::Web
