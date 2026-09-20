#include "pocket_daily/web/PrivateApPolicy.h"

#include <Arduino.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_system.h>
#include <esp_task_wdt.h>

namespace PocketDaily::Web {
namespace {

// Private-AP startup leaves a power-loss-durable trail on the SD card. The X3
// hangs hard during web-server startup at its tightest heap (no crash report,
// RTC breadcrumb lost on the power cycle needed to recover), so each step is
// flushed to this file and read back over File Transfer afterwards. Diagnostic
// only; cheap one-time writes on the Sync path.
constexpr const char* AP_BOOT_LOG_PATH = "/nearby_ap_log.txt";

// NimBLE-Arduino needs a sizeable contiguous working set while it creates the
// controller, host task, GATT database and advertising buffers.  The X3 has no
// PSRAM, so refuse the transition before framework allocations can abort when
// a reader cache has left the internal heap fragmented.
constexpr uint32_t NEARBY_START_MIN_FREE = 64U * 1024U;
constexpr uint32_t NEARBY_START_MIN_BLOCK = 32U * 1024U;
// The preflight is measured before NimBLE allocates its controller, host task
// and GATT database.  Guard the resulting steady state separately so a future
// library/configuration change fails back to Pocket Daily instead of reaching
// pairing with too little headroom and resetting the reader.
constexpr uint32_t NEARBY_READY_MIN_FREE = 20U * 1024U;
constexpr uint32_t NEARBY_READY_MIN_BLOCK = 8U * 1024U;
// Measured X3 STA/AP operation leaves roughly 22-27 KB free after the Wi-Fi
// driver starts. The Pocket profile omits WebDAV, WebSocket, discovery and
// captive-portal services; the full profile retains them and gets the larger
// guard. Upload buffers are allocated lazily after either gate.
constexpr uint32_t POCKET_WEB_START_MIN_FREE = 18U * 1024U;
constexpr uint32_t POCKET_WEB_START_MIN_BLOCK = 8U * 1024U;
constexpr uint32_t FULL_WEB_START_MIN_FREE = 20U * 1024U;
constexpr uint32_t FULL_WEB_START_MIN_BLOCK = 10U * 1024U;

}  // namespace

bool nearbyStartAllowed(const uint32_t freeHeap, const uint32_t largestBlock) {
  return freeHeap >= NEARBY_START_MIN_FREE && largestBlock >= NEARBY_START_MIN_BLOCK;
}

bool nearbyReadyAllowed(const uint32_t freeHeap, const uint32_t largestBlock) {
  return freeHeap >= NEARBY_READY_MIN_FREE && largestBlock >= NEARBY_READY_MIN_BLOCK;
}

bool webStartAllowed(const bool lightweightProfile, const uint32_t freeHeap, const uint32_t largestBlock) {
  const uint32_t minFree = lightweightProfile ? POCKET_WEB_START_MIN_FREE : FULL_WEB_START_MIN_FREE;
  const uint32_t minBlock = lightweightProfile ? POCKET_WEB_START_MIN_BLOCK : FULL_WEB_START_MIN_BLOCK;
  return freeHeap >= minFree && largestBlock >= minBlock;
}

Profile selectProfile(const bool privateAp, const bool deviceIsX3) {
  if (privateAp) return Profile::POCKET_SYNC;
  return deviceIsX3 ? Profile::FILE_TRANSFER : Profile::FULL;
}

bool armPrivateApWatchdog() {
  // The Arduino loop task is not watched by default. Nearby Sync runs at
  // the X3's tightest heap point and previously could retain a dead
  // Hotspot Mode frame forever if a network handler stopped returning.
  // Enrol it only for this bounded session; long transfer handlers already
  // reset the task watchdog while making progress.
  enableLoopWDT();
  // WebServer bounds each chunk send to HTTP_MAX_SEND_WAIT (5 s) waiting
  // for the client ACK. On a weak link the exact-screen preview's 4 KiB
  // sends approach that, and with the loop task watchdog also at 5 s the
  // two race and reset the reader mid-preview (observed breadcrumb
  // nearby:screen-preview). Widen the watchdog for this bounded private-AP
  // session so a legitimate slow send cannot trip it, while a true hang is
  // still caught. Only the loop task is subscribed here (idle task is not
  // watched), and Nearby Sync always exits through a chip restart, which
  // restores the sdkconfig 5 s default.
  const esp_task_wdt_config_t apWdt = {.timeout_ms = 12000, .idle_core_mask = 0, .trigger_panic = true};
  return esp_task_wdt_reconfigure(&apWdt) == ESP_OK;
}

void apBootLog(const char* step, const bool reset) {
  if (reset && Storage.exists(AP_BOOT_LOG_PATH)) Storage.remove(AP_BOOT_LOG_PATH);
  HalFile f = Storage.open(AP_BOOT_LOG_PATH, O_WRONLY | O_CREAT | O_APPEND);
  if (!f) return;
  char line[96];
  const int n = snprintf(line, sizeof(line), "%lu %s heap=%u largest=%u\n", (unsigned long)millis(), step,
                         (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  if (n > 0) f.write(reinterpret_cast<const uint8_t*>(line), (size_t)n);
  f.close();  // flush each step so a hard hang still leaves the trail
}

void generatePrivateApCredentials(const char* deviceId, char* ssidOut, const size_t ssidLen, char* passOut,
                                  const size_t passLen) {
  const uint32_t secretA = esp_random();
  const uint32_t secretB = esp_random();
  snprintf(ssidOut, ssidLen, "Pocket-%.4s", deviceId + 4);
  snprintf(passOut, passLen, "%08lX%04lX", static_cast<unsigned long>(secretA),
           static_cast<unsigned long>(secretB & 0xFFFFU));
}

}  // namespace PocketDaily::Web
