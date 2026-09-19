#include "NetHealth.h"

#include <cstdio>
#include <cstring>

#ifdef ENABLE_DEV_REMOTE_FLASH
#include <Arduino.h>
#include <HalStorage.h>
#include <WiFi.h>
#endif

namespace PocketDaily::NetHealth {

const char* reasonName(int reason) {
  switch (reason) {
    case 1:
      return "unspecified";
    case 2:
      return "auth_expire";
    case 4:
      return "assoc_expire";
    case 8:
      return "assoc_leave";
    case 9:
      return "assoc_not_authed";
    case 15:
      return "4way_handshake_timeout";
    case 200:
      return "beacon_timeout";
    case 201:
      return "no_ap_found";
    case 202:
      return "auth_fail";
    case 203:
      return "assoc_fail";
    case 204:
      return "handshake_fail";
    case 205:
      return "connection_fail";
    default:
      return "other";
  }
}

int formatHeartbeat(char* out, size_t cap, uint32_t uptimeS, uint32_t freeHeap, uint32_t maxBlock, int32_t rssi) {
  return std::snprintf(out, cap, "H %lu %lu %lu %ld", static_cast<unsigned long>(uptimeS),
                       static_cast<unsigned long>(freeHeap), static_cast<unsigned long>(maxBlock),
                       static_cast<long>(rssi));
}

int formatEvent(char* out, size_t cap, uint32_t uptimeS, const char* tag, int detail) {
  return std::snprintf(out, cap, "E %lu %s %d", static_cast<unsigned long>(uptimeS), tag, detail);
}

#ifdef ENABLE_DEV_REMOTE_FLASH

namespace {

constexpr char LOG_PATH[] = "/.crosspoint/net-health.log";
constexpr char LOG_OLD[] = "/.crosspoint/net-health.old";
constexpr uint32_t ROTATE_BYTES = 64 * 1024;
constexpr uint32_t HEARTBEAT_PERIOD_MS = 60 * 1000;

// Bounded append; drops the line entirely on failure (never block callers -
// the paths being instrumented are the ones that die). Same pattern as the
// private-AP boot log: flush per line so a hard hang still leaves the trail.
void appendLine(const char* line) {
  if (Storage.exists(LOG_PATH)) {
    HalFile probe = Storage.open(LOG_PATH);
    const bool rotate = probe && !probe.isDirectory() && static_cast<uint32_t>(probe.size()) > ROTATE_BYTES;
    if (probe) probe.close();
    if (rotate) {
      Storage.remove(LOG_OLD);
      Storage.rename(LOG_PATH, LOG_OLD);
    }
  }
  HalFile file = Storage.open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND);
  if (!file) return;
  file.write(reinterpret_cast<const uint8_t*>(line), strlen(line));
  file.write(reinterpret_cast<const uint8_t*>("\n"), 1);
  file.close();
}

void logEvent(const char* tag, int detail) {
  char line[96];
  formatEvent(line, sizeof(line), millis() / 1000, tag, detail);
  appendLine(line);
}

void heartbeatTask(void*) {
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_PERIOD_MS));
    char line[96];
    formatHeartbeat(line, sizeof(line), millis() / 1000, ESP.getFreeHeap(), ESP.getMaxAllocHeap(), WiFi.RSSI());
    appendLine(line);
  }
}

void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      logEvent("sta_disc", static_cast<int>(info.wifi_sta_disconnected.reason));
      break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      logEvent("sta_conn", 0);
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      logEvent("got_ip", 0);
      break;
    case ARDUINO_EVENT_WIFI_STA_LOST_IP:
      logEvent("lost_ip", 0);
      break;
    default:
      break;
  }
}

bool gStarted = false;

}  // namespace

void begin() {
  if (gStarted) return;
  gStarted = true;
  WiFi.onEvent(onWiFiEvent);
  logEvent("boot", 0);
  // Small stack: the task only formats one line and appends.
  xTaskCreate(heartbeatTask, "net-health", 3072, nullptr, 1, nullptr);
}

void note(const char* tag, int detail) { logEvent(tag, detail); }

size_t readTail(HalFile file, char* out, size_t cap) {
  const size_t size = file.size();
  const size_t start = size > cap - 1 ? size - (cap - 1) : 0;
  if (start && !file.seek(start)) {
    file.close();
    return 0;
  }
  const int n = file.read(reinterpret_cast<uint8_t*>(out), cap - 1);
  file.close();
  if (n <= 0) return 0;
  out[n] = '\0';
  return static_cast<size_t>(n);
}

size_t tail(char* out, size_t cap) {
  // Prefer the live file; fall back to the rotated one when absent.
  HalFile live = Storage.open(LOG_PATH);
  if (live && !live.isDirectory()) {
    return readTail(std::move(live), out, cap);
  }
  if (live) live.close();
  HalFile old = Storage.open(LOG_OLD);
  if (old && !old.isDirectory()) {
    return readTail(std::move(old), out, cap);
  }
  if (old) old.close();
  return 0;
}

#endif  // ENABLE_DEV_REMOTE_FLASH

}  // namespace PocketDaily::NetHealth
