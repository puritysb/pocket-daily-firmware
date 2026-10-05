#include "pocket_daily/web/PocketStatus.h"

#include <ArduinoJson.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <Memory.h>
#include <WiFi.h>

#include "pocket_daily/BuildFailureLog.h"
#include "pocket_daily/FirmwareIdentity.h"
#include "pocket_daily/ReaderPerf.h"
#include "pocket_daily/nearby_sync/ExchangeWindow.h"
#include "pocket_daily/web/UploadStreamServer.h"

namespace PocketDaily::Web {
namespace {
// Measured X3 private AP: ~6-7 KB free heap. Serving a 53 KB diagnostic file
// there tripped the task watchdog inside lwIP (crash breadcrumb
// nearby:screen-preview, a route since removed). Below this free-heap floor the
// reader reports diagnostics as unavailable and answers 503 so the companion
// never queues dozens of requests at a starved reader; transfers remain.
constexpr uint32_t DIAGNOSTIC_MIN_FREE_HEAP = 10U * 1024U;
}  // namespace

bool diagnosticsAffordable() { return ESP.getFreeHeap() >= DIAGNOSTIC_MIN_FREE_HEAP; }

String buildStatusJson(const StatusInputs& in) {
  // Get correct IP based on AP vs STA mode
  const String ipAddr = in.apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();

  JsonDocument doc;
  char deviceId[9];
  snprintf(deviceId, sizeof(deviceId), "%08lX", static_cast<unsigned long>(ESP.getEfuseMac() & 0xFFFFFFFFUL));
  doc["deviceID"] = deviceId;
  doc["sessionEnd"] = isSyncProfile(in.profile);
  doc["publicationReceipt"] = 1;
  doc["contentPresentation"] = in.contentPresentation;
  // Home / Daily Brief in Sync (docs/pocket-screen-present-v1.md); older
  // firmware omits the key and the companion must not call the routes.
  doc["articleLibrary"] = 1;
  doc["transferControl"] = 1;
  if (in.screenPresentation) doc["screenPresentation"] = 1;
  // Pocket Daily profile endpoints (docs/pocket-profile-v1.md) exist on Sync.
  if (isSyncProfile(in.profile)) doc["pocketProfile"] = 1;
  // POST /api/pocket/v1/glance (docs/pocket-glance-v1.md) sits beside it.
  if (isSyncProfile(in.profile)) doc["pocketGlance"] = 1;
  // GET /api/pocket/v1/content/file: read-only published revision files.
  doc["contentRead"] = 1;
  doc["version"] = CROSSPOINT_VERSION;
  doc["firmwareLineage"] = PocketDaily::FirmwareIdentity::LINEAGE;
  doc["crossPointBase"] = PocketDaily::FirmwareIdentity::CROSSPOINT_BASE;
  doc["ip"] = ipAddr;
  doc["mode"] = in.apMode ? "AP" : "STA";
  doc["rssi"] = in.apMode ? 0 : WiFi.RSSI();
  doc["freeHeap"] = ESP.getFreeHeap();
  doc["totalHeap"] = ESP.getHeapSize();
  // 2: list/delete/storage plus piece download (docs/reader-files.md).
  if (isSyncProfile(in.profile)) doc["readerFiles"] = 2;
  // GET/POST /api/pocket/v1/reading (docs/reading-progress-v1.md), Sync profiles only.
  if (isSyncProfile(in.profile)) doc["readingProgress"] = 1;
  doc["uptime"] = millis() / 1000;
  doc["device"] = gpio.deviceIsX3() ? "X3" : "X4";
  // Diagnoses a silent reboot during private-AP startup: the last runtime
  // checkpoint from the boot that just ended, readable over the LAN even when
  // no crash report was written (clean returnToLaunchOrigin).
  doc["lastBootBreadcrumb"] = HalSystem::getPreviousBootBreadcrumb();
  doc["lastResetReason"] = HalSystem::getResetReasonName();
  // Reading-sync exchange windows since power-on (docs/reading-sync-ble-v1.md, "Memory").
  if (const auto* sync = Pocket::NearbySync::Window::statistics()) {
    using namespace Pocket::NearbySync::Window;
    JsonObject w = doc["readSync"].to<JsonObject>();
    w["opened"] = sync->opened;
    w["skipped"] = sync->skipped;
    w["refused"] = sync->refused;
    w["connections"] = sync->connections;
    w["lists"] = sync->lists;
    w["offers"] = sync->offers;
    w["lastTrigger"] = triggerName(static_cast<Trigger>(sync->lastTrigger));
    w["lastGate"] = gateName(static_cast<Gate>(sync->lastGate));
    w["lastClose"] = closeReasonName(static_cast<CloseReason>(sync->lastClose));
    w["startFree"] = sync->startFree;
    w["startBlock"] = sync->startBlock;
    w["openFree"] = sync->openFree;
    w["openBlock"] = sync->openBlock;
    w["minFree"] = sync->minFree;
    w["minBlock"] = sync->minBlock;
    w["closedFree"] = sync->closedFree;
    w["closedBlock"] = sync->closedBlock;
  }
  if (in.stream && in.stream->listening()) {
    doc["uploadStreamPort"] = in.stream->port();
    // The stream keeps an interrupted staging file and accepts `Resume: 1`.
    doc["uploadStreamResume"] = true;
    doc["uploadStreamWindow"] = PocketDaily::UploadStream::FLOW_WINDOW_BYTES;
  }
  // A sync heartbeat is not permission to start SD diagnostics or frame
  // downloads. Explicit diagnostic endpoints remain available outside this
  // automatic advertisement; content redraw uses its own verified receipt.
  const bool affordable = !isSyncProfile(in.profile) && diagnosticsAffordable();
  doc["diagnosticsAffordable"] = affordable;
  if (affordable && Storage.exists("/crash_report.txt")) {
    HalFile report = Storage.open("/crash_report.txt");
    doc["crashReportAvailable"] = static_cast<bool>(report);
    doc["crashReportBytes"] = report ? report.size() : 0;
    if (report) report.close();
  } else {
    doc["crashReportAvailable"] = false;
    doc["crashReportBytes"] = 0;
  }
  // The last EPUB chapter the reader failed to build (docs/build-failure-log.md):
  // one 96-byte SD read, under the same SD-diagnostics gate as the crash report.
  // Absent when nothing was recorded or diagnostics are not affordable.
  PocketDaily::BuildFailureLog::Entry buildFailure;
  if (affordable && PocketDaily::BuildFailureLog::load(buildFailure)) {
    namespace BuildLog = PocketDaily::BuildFailureLog;
    JsonObject failure = doc["lastBuildError"].to<JsonObject>();
    failure["book"] = buildFailure.book;
    failure["spine"] = buildFailure.spine;
    failure["step"] = BuildLog::stepName(buildFailure.step);
    const char* detail = BuildLog::detailName(buildFailure.step, buildFailure.detail);
    failure["detail"] = detail ? detail : nullptr;
    failure["count"] = buildFailure.count;
    failure["freeHeap"] = buildFailure.freeHeap;
    failure["largestBlock"] = buildFailure.largestBlock;
    failure["uptime"] = buildFailure.uptimeSec;
    failure["version"] = buildFailure.version;
  }
  // Reader page-turn timings (docs/reader-perf.md): one ~0.85 KB SD read under the
  // same gate. Stage values are milliseconds in `fields` order; `recent` is newest first.
  if (affordable) {
    namespace Perf = PocketDaily::ReaderPerf;
    // Heap, not stack: a snapshot is ~0.9 KB (handler stack budget).
    auto perf = makeUniqueNoThrow<Perf::Snapshot>();
    if (perf && Perf::load(*perf) && perf->totals.turns > 0) {
      JsonObject rp = doc["readerPerf"].to<JsonObject>();
      rp["format"] = Perf::FORMAT_VERSION;
      rp["version"] = perf->version;
      rp["turns"] = perf->totals.turns;
      rp["fields"] = Perf::fieldNames();
      char line[160];
      if (Perf::formatTotals(perf->totals, false, line, sizeof(line))) rp["avg"] = line;
      if (Perf::formatTotals(perf->totals, true, line, sizeof(line))) rp["max"] = line;
      rp["minFreeHeap"] = perf->totals.minFreeHeap;
      rp["minLargestBlock"] = perf->totals.minLargestBlock;
      JsonArray recent = rp["recent"].to<JsonArray>();
      for (size_t i = 0; const Perf::TurnRecord* record = perf->recent(i); ++i) {
        if (Perf::formatRecord(*record, line, sizeof(line))) recent.add(line);
      }
    }
  }
  // Live Studio v1 capability advertisement. Absent on older firmware; the
  // companion treats a missing object as a legacy poll-only reader.
  {
    JsonObject live = doc["liveStudio"].to<JsonObject>();
    live["mode"] = (in.live.push && !in.live.suspended) ? "push" : "poll";
    if (in.live.push && !in.live.suspended) live["wsPort"] = in.live.wsPort;
  }

  String json;
  serializeJson(doc, json);
  return json;
}

}  // namespace PocketDaily::Web
