#include "pocket_daily/web/PocketStatus.h"

#include <ArduinoJson.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <WiFi.h>

#include "pocket_daily/PocketScreenPreview.h"
#include "pocket_daily/web/UploadStreamServer.h"

namespace PocketDaily::Web {
namespace {
// Measured X3 private AP: ~6-7 KB free heap. Serving a 53 KB screen preview
// there tripped the task watchdog inside lwIP (crash breadcrumb
// nearby:screen-preview). Below this free-heap floor the reader reports the
// diagnostics as unavailable and answers 503 so the companion never queues
// dozens of requests at a starved reader; transfers remain available.
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
  doc["sessionEnd"] = in.profile == Profile::POCKET_SYNC && in.apMode;
  doc["contentPresentation"] = in.contentPresentation;
  // Pocket Daily profile endpoints (docs/pocket-profile-v1.md) exist on Sync.
  if (isSyncProfile(in.profile)) doc["pocketProfile"] = 1;
  // POST /api/pocket/v1/glance (docs/pocket-glance-v1.md) sits beside it.
  if (isSyncProfile(in.profile)) doc["pocketGlance"] = 1;
  // GET /api/pocket/v1/content/file: read-only published revision files.
  doc["contentRead"] = 1;
  doc["version"] = CROSSPOINT_VERSION;
  doc["ip"] = ipAddr;
  doc["mode"] = in.apMode ? "AP" : "STA";
  doc["rssi"] = in.apMode ? 0 : WiFi.RSSI();
  doc["freeHeap"] = ESP.getFreeHeap();
  doc["uptime"] = millis() / 1000;
  doc["device"] = gpio.deviceIsX3() ? "X3" : "X4";
  // Diagnoses a silent reboot during private-AP startup: the last runtime
  // checkpoint from the boot that just ended, readable over the LAN even when
  // no crash report was written (clean returnToLaunchOrigin).
  doc["lastBootBreadcrumb"] = HalSystem::getPreviousBootBreadcrumb();
  doc["lastResetReason"] = HalSystem::getResetReasonName();
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
  bool screenPreviewAvailable = false;
  int screenPreviewBytes = 0;
  if (in.profile == Profile::POCKET_SYNC && affordable && Storage.exists(PocketDaily::SCREEN_PREVIEW_PATH)) {
    HalFile preview = Storage.open(PocketDaily::SCREEN_PREVIEW_PATH);
    screenPreviewAvailable = static_cast<bool>(preview) && !preview.isDirectory();
    screenPreviewBytes = preview ? preview.size() : 0;
    if (preview) preview.close();
  }
  doc["screenPreviewAvailable"] = screenPreviewAvailable;
  doc["screenPreviewBytes"] = screenPreviewBytes;
  if (affordable && Storage.exists("/crash_report.txt")) {
    HalFile report = Storage.open("/crash_report.txt");
    doc["crashReportAvailable"] = static_cast<bool>(report);
    doc["crashReportBytes"] = report ? report.size() : 0;
    if (report) report.close();
  } else {
    doc["crashReportAvailable"] = false;
    doc["crashReportBytes"] = 0;
  }
  // Live Studio v1 capability advertisement. Absent on older firmware; the
  // companion treats a missing object as a legacy poll-only reader.
  {
    JsonObject live = doc["liveStudio"].to<JsonObject>();
    live["mode"] = (in.live.push && !in.live.suspended) ? "push" : "poll";
    if (in.live.push && !in.live.suspended) live["wsPort"] = in.live.wsPort;
    live["frameStream"] = in.live.push && !in.live.suspended;
    live["uiPacks"] = true;  // LS-3: .uipack apply
    live["activePack"] = in.live.activePackName[0] ? in.live.activePackName : nullptr;
    live["activePackVersion"] = in.live.activePackVersion[0] ? in.live.activePackVersion : nullptr;
  }

  String json;
  serializeJson(doc, json);
  return json;
}

}  // namespace PocketDaily::Web
