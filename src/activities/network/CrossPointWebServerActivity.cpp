#include "CrossPointWebServerActivity.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalSystem.h>
#include <I18n.h>
#include <Memory.h>
#include <WiFi.h>
#include <esp_system.h>
#include <esp_task_wdt.h>

#include <cstddef>

#include "MappedInputManager.h"
#include "NetworkModeSelectionActivity.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "WifiSelectionActivity.h"
#include "activities/network/CalibreConnectActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/QrUtils.h"

namespace {
// AP Mode configuration
constexpr const char* AP_SSID = "CrossPoint-Reader";
constexpr const char* AP_PASSWORD = nullptr;  // Open network for ease of use
constexpr const char* AP_HOSTNAME = "crosspoint";
constexpr uint8_t AP_CHANNEL = 1;
constexpr uint8_t AP_MAX_CONNECTIONS = 4;
constexpr uint8_t PRIVATE_AP_MAX_CONNECTIONS = 1;
constexpr uint16_t PRIVATE_AP_LEASE_SECONDS = 300;
constexpr unsigned long BLE_HANDOFF_DELAY_MS = 900;
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
constexpr int QR_CODE_WIDTH = 198;
constexpr int QR_CODE_HEIGHT = 198;

// DNS server for captive portal (redirects all DNS queries to our IP)
DNSServer* dnsServer = nullptr;
constexpr uint16_t DNS_PORT = 53;

void stopDnsServer() {
  if (!dnsServer) return;

  dnsServer->stop();
  delete dnsServer;
  dnsServer = nullptr;
}

void restartMdns(const char* hostname, const char* tag) {
  MDNS.end();
  if (MDNS.begin(hostname)) {
    MDNS.addService("crosspoint", "tcp", 80);
    LOG_DBG(tag, "mDNS started: http://%s.local/", hostname);
  } else {
    LOG_DBG(tag, "WARNING: mDNS failed to start");
  }
}

// 0..4 bars from RSSI (dBm), with 3 dBm hysteresis on currentBars to suppress flicker.
int barsForRssi(int rssi, int currentBars) {
  static constexpr int RISE_DBM[] = {-85, -75, -65, -55};
  static constexpr int FALL_DBM[] = {-88, -78, -68, -58};
  int bars = std::clamp(currentBars, 0, 4);
  while (bars < 4 && rssi >= RISE_DBM[bars]) bars++;
  while (bars > 0 && rssi < FALL_DBM[bars - 1]) bars--;
  return bars;
}
}  // namespace

void CrossPointWebServerActivity::onEnter() {
  Activity::onEnter();

  LOG_DBG("WEBACT", "Free heap at onEnter: %d bytes", ESP.getFreeHeap());

  // Reset state
  state = WebServerActivityState::MODE_SELECTION;
  networkMode = NetworkMode::JOIN_NETWORK;
  isApMode = false;
  privateApMode = false;
  privateApSsid[0] = '\0';
  privateApPassword[0] = '\0';
  nearbyHandoffAt = 0;
  privateApStartedAt = 0;
  lastNearbyAuthenticated = false;
  nearbyStartResult.store(NearbyStartResult::IDLE, std::memory_order_release);
  nearbyCancelRequested.store(false, std::memory_order_release);
  nearbyStartTask = nullptr;
  connectedIP.clear();
  connectedSSID.clear();
  lastHandleClientTime = 0;
  requestUpdate();

  if (launchMode == WebServerLaunchMode::POCKET_NEARBY_SYNC) {
    LOG_DBG("WEBACT", "Launching Pocket Nearby Sync directly...");
    startNearbySync();
    return;
  }

  // Launch network mode selection subactivity
  LOG_DBG("WEBACT", "Launching NetworkModeSelectionActivity...");
  startActivityForResult(std::make_unique<NetworkModeSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             returnToLaunchOrigin();
                           } else {
                             onNetworkModeSelected(std::get<NetworkModeResult>(result.data).mode);
                           }
                         });
}

void CrossPointWebServerActivity::returnToLaunchOrigin() {
  if (launchMode == WebServerLaunchMode::POCKET_NEARBY_SYNC)
    activityManager.goToPocketDaily();
  else
    onGoHome();
}

void CrossPointWebServerActivity::onExit() {
  Activity::onExit();

  LOG_DBG("WEBACT", "Free heap at onExit start: %d bytes", ESP.getFreeHeap());

  state = WebServerActivityState::SHUTTING_DOWN;

  // Nearby Sync always leaves through a chip restart. Do not synchronously
  // dismantle NimBLE, the HTTP server, DNS and the AP first: on X3 the AP can
  // disappear while WiFi.softAPdisconnect(true) remains wedged, leaving the
  // retained Hotspot Mode frame on screen and every button unresponsive.
  // ESP.restart() tears all radio tasks down as part of reset, and doing it
  // before any driver shutdown keeps this user action bounded.
  if (launchMode == WebServerLaunchMode::POCKET_NEARBY_SYNC) {
    HalSystem::setCrashBreadcrumb("nearby:exit-restart");
    silentRestartToPocketDaily();
    return;  // ESP.restart() does not return.
  }

  // Stop accepting work before tearing down either radio.  Wi-Fi activities
  // normally reboot on exit, but explicitly closing the synchronous server
  // keeps a pending client from delaying that hand-off and makes this cleanup
  // correct if the reboot policy changes later.
  if (webServer) {
    webServer->stop();
    webServer.reset();
  }
  // Normal Back handling waits for the worker before leaving. This bounded
  // fallback also protects unusual global transitions from destroying the
  // activity while NimBLE still holds a pointer to its Service.
  nearbyCancelRequested.store(true, std::memory_order_release);
  for (int i = 0; nearbyStartResult.load(std::memory_order_acquire) == NearbyStartResult::RUNNING && i < 1500; ++i) {
    delay(10);
  }
  nearbySync.end();
  stopDnsServer();
  MDNS.end();

  // Skip reboot if WiFi was never activated (e.g. user backed out of mode selection).
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    if (isApMode) {
      WiFi.softAPdisconnect(true);
    } else {
      WiFi.disconnect(false);
    }
    delay(30);
    silentRestart();
  }

  LOG_DBG("WEBACT", "Free heap at onExit end: %d bytes", ESP.getFreeHeap());
}

void CrossPointWebServerActivity::onNetworkModeSelected(const NetworkMode mode) {
  const char* modeName = "Join Network";
  if (mode == NetworkMode::CONNECT_CALIBRE) {
    modeName = "Connect to Calibre";
  } else if (mode == NetworkMode::CREATE_HOTSPOT) {
    modeName = "Create Hotspot";
  }
  LOG_DBG("WEBACT", "Network mode selected: %s", modeName);

  networkMode = mode;
  isApMode = (mode == NetworkMode::CREATE_HOTSPOT);
  privateApMode = false;

  if (mode == NetworkMode::CONNECT_CALIBRE) {
    startActivityForResult(
        std::make_unique<CalibreConnectActivity>(renderer, mappedInput), [this](const ActivityResult& result) {
          state = WebServerActivityState::MODE_SELECTION;

          startActivityForResult(std::make_unique<NetworkModeSelectionActivity>(renderer, mappedInput),
                                 [this](const ActivityResult& result) {
                                   if (result.isCancelled) {
                                     returnToLaunchOrigin();
                                   } else {
                                     onNetworkModeSelected(std::get<NetworkModeResult>(result.data).mode);
                                   }
                                 });
        });
    return;
  }

  if (mode == NetworkMode::JOIN_NETWORK) {
    // STA mode - launch WiFi selection
    LOG_DBG("WEBACT", "Turning on WiFi (STA mode)...");
    WiFi.mode(WIFI_STA);

    state = WebServerActivityState::WIFI_SELECTION;
    LOG_DBG("WEBACT", "Launching WifiSelectionActivity...");
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& wifi = std::get<WifiResult>(result.data);
                               connectedIP = wifi.ip;
                               connectedSSID = wifi.ssid;
                             }
                             onWifiSelectionComplete(!result.isCancelled);
                           });
  } else {
    // AP mode - start access point
    state = WebServerActivityState::AP_STARTING;
    requestUpdate();
    startAccessPoint();
  }
}

void CrossPointWebServerActivity::onWifiSelectionComplete(const bool connected) {
  LOG_DBG("WEBACT", "WifiSelectionActivity completed, connected=%d", connected);

  if (connected) {
    // Get connection info before exiting subactivity
    isApMode = false;

    // X3 exposes the IP and QR code directly. mDNS costs about 5.6 KB on this
    // no-PSRAM board and is not worth running beside browser file transfer.
    if (!gpio.deviceIsX3()) restartMdns(AP_HOSTNAME, "WEBACT");

    // Start the web server
    startWebServer();
  } else {
    // User cancelled - go back to mode selection
    state = WebServerActivityState::MODE_SELECTION;

    startActivityForResult(std::make_unique<NetworkModeSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             if (result.isCancelled) {
                               returnToLaunchOrigin();
                             } else {
                               onNetworkModeSelected(std::get<NetworkModeResult>(result.data).mode);
                             }
                           });
  }
}

void CrossPointWebServerActivity::startNearbySync() {
  state = WebServerActivityState::NEARBY_STARTING;
  // Paint acknowledgement before entering the radio stack.  E-ink refresh is
  // slow and NimBLE initialization is synchronous, so a deferred paint made a
  // healthy transition look like an ignored button press and left no useful
  // feedback if the radio ran out of heap.
  requestUpdateAndWait();

  // Nearby Sync owns the radio in this activity. Do not let a previous station
  // session coexist with NimBLE on the no-PSRAM device.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }

  // The multilingual reader family keeps interval tables, decompression pages
  // and glyph caches in internal RAM.  It is reloadable from SD and must not be
  // resident while BLE is active (docs/nearby-sync-v1.md memory gate).  Hold the
  // render mutex while removing it so a queued paint cannot use the family
  // after it has been unloaded.
  const uint32_t heapBeforeFontRelease = ESP.getFreeHeap();
  {
    RenderLock fontRenderLock(*this);
    sdFontSystem.releaseLoaded(renderer);
  }
  const uint32_t heapAfterFontRelease = ESP.getFreeHeap();
  const uint32_t largestBlock = ESP.getMaxAllocHeap();
  LOG_INF("NEARBY", "preflight heap=%lu->%lu largest=%lu", static_cast<unsigned long>(heapBeforeFontRelease),
          static_cast<unsigned long>(heapAfterFontRelease), static_cast<unsigned long>(largestBlock));

  if (heapAfterFontRelease < NEARBY_START_MIN_FREE || largestBlock < NEARBY_START_MIN_BLOCK) {
    LOG_ERR("NEARBY", "start refused: heap free=%lu largest=%lu", static_cast<unsigned long>(heapAfterFontRelease),
            static_cast<unsigned long>(largestBlock));
    returnToLaunchOrigin();
    return;
  }

  nearbyModel = gpio.deviceIsX3() ? "X3" : "X4";
  nearbyCancelRequested.store(false, std::memory_order_release);
  nearbyStartResult.store(NearbyStartResult::RUNNING, std::memory_order_release);
  if (xTaskCreate(nearbyStartTaskTrampoline, "PocketBLEStart", 6144, this, 1, &nearbyStartTask) != pdPASS) {
    nearbyStartResult.store(NearbyStartResult::FAILED, std::memory_order_release);
    nearbyStartTask = nullptr;
    LOG_ERR("WEBACT", "Could not create Nearby Sync startup task");
  }
}

void CrossPointWebServerActivity::nearbyStartTaskTrampoline(void* context) {
  auto* activity = static_cast<CrossPointWebServerActivity*>(context);
  const bool started = activity->nearbySync.begin(activity->nearbyModel, CROSSPOINT_VERSION);
  activity->nearbyStartResult.store(started ? NearbyStartResult::SUCCEEDED : NearbyStartResult::FAILED,
                                    std::memory_order_release);
  vTaskDelete(nullptr);
}

void CrossPointWebServerActivity::handleNearbyStartup() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    nearbyCancelRequested.store(true, std::memory_order_release);
    requestUpdate();
  }

  const NearbyStartResult result = nearbyStartResult.load(std::memory_order_acquire);
  if (result == NearbyStartResult::RUNNING) return;
  nearbyStartTask = nullptr;

  if (result == NearbyStartResult::SUCCEEDED && !nearbyCancelRequested.load(std::memory_order_acquire)) {
    const uint32_t freeHeap = ESP.getFreeHeap();
    const uint32_t largestBlock = ESP.getMaxAllocHeap();
    LOG_INF("NEARBY", "ready heap=%lu largest=%lu", static_cast<unsigned long>(freeHeap),
            static_cast<unsigned long>(largestBlock));
    if (freeHeap < NEARBY_READY_MIN_FREE || largestBlock < NEARBY_READY_MIN_BLOCK) {
      LOG_ERR("NEARBY", "ready refused: heap free=%lu largest=%lu", static_cast<unsigned long>(freeHeap),
              static_cast<unsigned long>(largestBlock));
      HalSystem::setCrashBreadcrumb("nearby:ready-low-memory");
      nearbySync.end();
      returnToLaunchOrigin();
      return;
    }
    HalSystem::setCrashBreadcrumb("nearby:ready-for-pairing");
    state = WebServerActivityState::NEARBY_READY;
    lastNearbyAuthenticated = nearbySync.isAuthenticated();
    requestUpdate();
    return;
  }

  if (result == NearbyStartResult::SUCCEEDED) nearbySync.end();
  if (result == NearbyStartResult::FAILED) LOG_ERR("WEBACT", "Nearby Sync failed to start");
  returnToLaunchOrigin();
}

void CrossPointWebServerActivity::handleNearbySync() {
  const bool authenticated = nearbySync.isAuthenticated();
  if (authenticated != lastNearbyAuthenticated) {
    lastNearbyAuthenticated = authenticated;
    requestUpdate();
  }

  if (state == WebServerActivityState::NEARBY_HANDOFF) {
    if (millis() - nearbyHandoffAt < BLE_HANDOFF_DELAY_MS) return;

    nearbySync.end();
    isApMode = true;
    privateApMode = true;
    state = WebServerActivityState::AP_STARTING;
    requestUpdate();
    startAccessPoint();
    return;
  }

  Pocket::NearbySync::Command command;
  if (!nearbySync.takeCommand(command)) return;

  switch (command.type) {
    case Pocket::NearbySync::CommandType::PING:
      nearbySync.notifyOk(command.requestId);
      break;
    case Pocket::NearbySync::CommandType::CANCEL:
      nearbySync.notifyOk(command.requestId);
      break;
    case Pocket::NearbySync::CommandType::START_AP: {
      const uint32_t secretA = esp_random();
      const uint32_t secretB = esp_random();
      snprintf(privateApSsid, sizeof(privateApSsid), "Pocket-%.4s", nearbySync.deviceId() + 4);
      snprintf(privateApPassword, sizeof(privateApPassword), "%08lX%04lX", static_cast<unsigned long>(secretA),
               static_cast<unsigned long>(secretB & 0xFFFFU));

      if (!nearbySync.notifyHotspot(command.requestId, privateApSsid, privateApPassword, "192.168.4.1", 80, 0,
                                    PRIVATE_AP_LEASE_SECONDS)) {
        nearbySync.notifyError(command.requestId, "NOT_SUBSCRIBED");
        break;
      }
      nearbyHandoffAt = millis();
      state = WebServerActivityState::NEARBY_HANDOFF;
      requestUpdate();
      break;
    }
    case Pocket::NearbySync::CommandType::NONE:
      break;
  }
}

void CrossPointWebServerActivity::startAccessPoint() {
  LOG_DBG("WEBACT", "Starting Access Point mode...");
  LOG_DBG("WEBACT", "Free heap before AP start: %d bytes", ESP.getFreeHeap());

  // Configure and start the AP
  WiFi.mode(WIFI_AP);
  delay(100);

  // Start soft AP
  bool apStarted;
  const char* ssid = privateApMode ? privateApSsid : AP_SSID;
  const char* password = privateApMode ? privateApPassword : AP_PASSWORD;
  const uint8_t maxConnections = privateApMode ? PRIVATE_AP_MAX_CONNECTIONS : AP_MAX_CONNECTIONS;
  if (password && strlen(password) >= 8) {
    apStarted = WiFi.softAP(ssid, password, AP_CHANNEL, false, maxConnections);
  } else {
    // Open network (no password)
    apStarted = WiFi.softAP(ssid, nullptr, AP_CHANNEL, false, maxConnections);
  }

  if (!apStarted) {
    LOG_ERR("WEBACT", "ERROR: Failed to start Access Point!");
    returnToLaunchOrigin();
    return;
  }

  delay(100);  // Wait for AP to fully initialize

  // Get AP IP address
  const IPAddress apIP = WiFi.softAPIP();
  char ipStr[16];
  snprintf(ipStr, sizeof(ipStr), "%d.%d.%d.%d", apIP[0], apIP[1], apIP[2], apIP[3]);
  connectedIP = ipStr;
  connectedSSID = ssid;
  if (privateApMode) privateApStartedAt = millis();

  LOG_DBG("WEBACT", "Access Point started!");
  LOG_DBG("WEBACT", "SSID: %s", ssid);
  LOG_DBG("WEBACT", "IP: %s", connectedIP.c_str());

  if (!privateApMode && !gpio.deviceIsX3()) {
    // Generic File Transfer remains discoverable and captive-portal friendly.
    // Pocket already received 192.168.4.1 over authenticated BLE, so these
    // services would only consume scarce X3 heap on its private AP.
    restartMdns(AP_HOSTNAME, "WEBACT");
    stopDnsServer();
    dnsServer = new (std::nothrow) DNSServer();
    if (dnsServer) {
      dnsServer->setErrorReplyCode(DNSReplyCode::NoError);
      dnsServer->start(DNS_PORT, "*", apIP);
      LOG_DBG("WEBACT", "DNS server started for captive portal");
    } else {
      LOG_ERR("WEBACT", "Could not allocate captive-portal DNS server");
    }
  }

  LOG_DBG("WEBACT", "Free heap after AP start: %d bytes", ESP.getFreeHeap());

  // Start the web server
  startWebServer();
}

void CrossPointWebServerActivity::startWebServer() {
  LOG_DBG("WEBACT", "Starting web server...");

  state = WebServerActivityState::SERVER_STARTING;
  requestUpdateAndWait();

  // X3/X4 have no PSRAM. A resident .cpfont keeps its interval tables,
  // decompression pages and glyph caches in internal RAM; leaving it loaded
  // while WebServer + WebSocketsServer allocate their TCP buffers reduced the
  // X3 to an 8 KB free heap (and a ~2 KB largest block), causing otherwise
  // valid uploads to lose their socket after roughly 128 KB. File Transfer
  // does not render reader text, so release the resident family while keeping
  // the saved selection. The next reader activity reloads it on demand (and
  // leaving this activity already performs a clean restart after Wi-Fi use).
  const uint32_t heapBeforeFontRelease = ESP.getFreeHeap();
  {
    RenderLock fontRenderLock(*this);
    sdFontSystem.releaseLoaded(renderer);
  }
  LOG_DBG("WEBACT", "Released resident SD font: heap %u -> %u (largest %u)", (unsigned)heapBeforeFontRelease,
          (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

  const auto profile = privateApMode ? CrossPointWebServerProfile::POCKET_SYNC
                                     : (gpio.deviceIsX3() ? CrossPointWebServerProfile::FILE_TRANSFER
                                                          : CrossPointWebServerProfile::FULL);
  const bool lightweightProfile = profile != CrossPointWebServerProfile::FULL;
  const uint32_t minFree = lightweightProfile ? POCKET_WEB_START_MIN_FREE : FULL_WEB_START_MIN_FREE;
  const uint32_t minBlock = lightweightProfile ? POCKET_WEB_START_MIN_BLOCK : FULL_WEB_START_MIN_BLOCK;
  if (ESP.getFreeHeap() < minFree || ESP.getMaxAllocHeap() < minBlock) {
    LOG_ERR("WEBACT", "Web server start refused: heap free=%u largest=%u", (unsigned)ESP.getFreeHeap(),
            (unsigned)ESP.getMaxAllocHeap());
    returnToLaunchOrigin();
    return;
  }

  // Create the web server instance
  webServer = makeUniqueNoThrow<CrossPointWebServer>(profile);
  if (!webServer) {
    LOG_ERR("WEBACT", "Could not allocate web server");
    returnToLaunchOrigin();
    return;
  }
  webServer->begin();

  if (webServer->isRunning()) {
    state = WebServerActivityState::SERVER_RUNNING;
    LOG_DBG("WEBACT", "Web server started successfully");
    if (privateApMode) {
      // The Arduino loop task is not watched by default. Nearby Sync runs at
      // the X3's tightest heap point and previously could retain a dead
      // Hotspot Mode frame forever if a network handler stopped returning.
      // Enrol it only for this bounded session; long transfer handlers already
      // reset the task watchdog while making progress.
      HalSystem::setCrashBreadcrumb("nearby:server-running");
      enableLoopWDT();
    }
    lastWifiBars = isApMode ? 0 : barsForRssi(WiFi.RSSI(), 0);

    // Force an immediate render since we're transitioning from a subactivity
    // that had its own rendering task. We need to make sure our display is shown.
    requestUpdate();
  } else {
    LOG_ERR("WEBACT", "ERROR: Failed to start web server!");
    webServer.reset();
    // Go back on error
    returnToLaunchOrigin();
  }
}

void CrossPointWebServerActivity::loop() {
  if (state == WebServerActivityState::NEARBY_STARTING) {
    handleNearbyStartup();
    return;
  }

  if (state == WebServerActivityState::NEARBY_READY || state == WebServerActivityState::NEARBY_HANDOFF) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      returnToLaunchOrigin();
      return;
    }
    handleNearbySync();
    return;
  }

  // Handle different states
  if (state == WebServerActivityState::SERVER_RUNNING) {
    // gpio.update() already ran once at the start of the global loop. Consume
    // its edge before any synchronous network work. Calling mappedInput.update
    // again in this activity clears pressedEvents and used to erase Back here.
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      LOG_INF("WEBACT", "Exit requested while web server is running");
      state = WebServerActivityState::SHUTTING_DOWN;
      returnToLaunchOrigin();
      return;
    }

    if (privateApMode && privateApStartedAt != 0 &&
        millis() - privateApStartedAt >= static_cast<unsigned long>(PRIVATE_AP_LEASE_SECONDS) * 1000UL) {
      LOG_INF("WEBACT", "Nearby Sync private AP lease expired");
      returnToLaunchOrigin();
      return;
    }

    // Handle DNS requests for captive portal (AP mode only)
    if (isApMode && dnsServer) {
      dnsServer->processNextRequest();
    }

    // STA mode: Monitor WiFi connection health
    if (!isApMode && webServer && webServer->isRunning()) {
      static unsigned long lastWifiCheck = 0;
      if (millis() - lastWifiCheck > 2000) {  // Check every 2 seconds
        lastWifiCheck = millis();
        const wl_status_t wifiStatus = WiFi.status();
        // Driver auto-reconnect handles retries; abandon (via onGoHome) only
        // after WIFI_ABANDON_MS, otherwise the activity freezes on a blip.
        bool repaint = false;
        if (wifiStatus != WL_CONNECTED) {
          if (consecutiveDisconnects == 0) {
            firstDisconnectAt = millis();
            repaint = true;
          }
          consecutiveDisconnects++;
          LOG_DBG("WEBACT", "WiFi not connected (status=%d, consecutive=%d, total=%lu ms)", wifiStatus,
                  consecutiveDisconnects, millis() - firstDisconnectAt);
          if (millis() - firstDisconnectAt > WIFI_ABANDON_MS) {
            LOG_DBG("WEBACT", "WiFi unavailable for >%lu s; returning to network selection", WIFI_ABANDON_MS / 1000UL);
            state = WebServerActivityState::SHUTTING_DOWN;
            returnToLaunchOrigin();
            return;
          }
        } else {
          if (consecutiveDisconnects > 0) {
            LOG_DBG("WEBACT", "WiFi recovered after %d failed checks (%lu ms)", consecutiveDisconnects,
                    millis() - firstDisconnectAt);
            repaint = true;
          }
          consecutiveDisconnects = 0;
          firstDisconnectAt = 0;
          const int rssi = WiFi.RSSI();
          if (rssi < -75) {
            LOG_DBG("WEBACT", "Warning: Weak WiFi signal: %d dBm", rssi);
          }
          const int bars = barsForRssi(rssi, lastWifiBars);
          if (bars != lastWifiBars) {
            lastWifiBars = bars;
            repaint = true;
          }
        }
        if (repaint) requestUpdate();
      }
    }

    // Handle web server requests - maximize throughput with watchdog safety
    if (webServer && webServer->isRunning()) {
      const unsigned long timeSinceLastHandleClient = millis() - lastHandleClientTime;

      // Log if there's a significant gap between handleClient calls (>100ms)
      if (lastHandleClientTime > 0 && timeSinceLastHandleClient > 100) {
        LOG_DBG("WEBACT", "WARNING: %lu ms gap since last handleClient", timeSinceLastHandleClient);
      }

      // Reset watchdog BEFORE processing - HTTP header parsing can be slow
      if (privateApMode) HalSystem::setCrashBreadcrumb("nearby:http-handle");
      esp_task_wdt_reset();

      // Service a small bounded batch, then return to the global loop so GPIO
      // is sampled again. Upload bodies are consumed by WebServer itself; 500
      // back-to-back calls only starved buttons while the server was idle.
      // A companion request already consumes its complete HTTP transaction in
      // one call. Return immediately afterward in private-AP mode so the next
      // global loop samples the physical buttons before accepting more work.
      // X3 also owns the incremental Pocket upload listener in Join Network
      // mode. Service one bounded network slice, then let the global loop
      // sample GPIO before reading the next 768-byte SD chunk.
      const int maxIterations = (privateApMode || gpio.deviceIsX3()) ? 1 : 8;
      for (int i = 0; i < maxIterations && webServer->isRunning(); i++) {
        webServer->handleClient();
        if ((i & 0x03) == 0x03) {
          esp_task_wdt_reset();
          yield();
        }
      }
      lastHandleClientTime = millis();
      if (privateApMode) HalSystem::setCrashBreadcrumb("nearby:server-idle");
    }
  }
}

void CrossPointWebServerActivity::render(RenderLock&&) {
  if (state == WebServerActivityState::NEARBY_STARTING || state == WebServerActivityState::NEARBY_READY ||
      state == WebServerActivityState::NEARBY_HANDOFF) {
    renderNearbySync();
    renderer.displayBuffer();
    return;
  }

  // Only render our own UI when server is running
  // Subactivities handle their own rendering
  if (state == WebServerActivityState::SERVER_RUNNING || state == WebServerActivityState::AP_STARTING ||
      state == WebServerActivityState::SERVER_STARTING) {
    renderer.clearScreen();
    const auto& metrics = UITheme::getInstance().getMetrics();
    const auto pageWidth = renderer.getScreenWidth();
    const auto pageHeight = renderer.getScreenHeight();

    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                   isApMode ? tr(STR_HOTSPOT_MODE) : tr(STR_FILE_TRANSFER), nullptr);

    if (state == WebServerActivityState::SERVER_RUNNING) {
      GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                        connectedSSID.c_str());
      renderServerRunning();
    } else {
      const auto height = renderer.getLineHeight(UI_10_FONT_ID);
      const auto top = (pageHeight - height) / 2;
      renderer.drawCenteredText(
          UI_10_FONT_ID, top,
          state == WebServerActivityState::AP_STARTING ? tr(STR_STARTING_HOTSPOT) : tr(STR_STARTING_FILE_TRANSFER));
    }
    renderer.displayBuffer();
  }
}

void CrossPointWebServerActivity::renderServerRunning() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 isApMode ? tr(STR_HOTSPOT_MODE) : tr(STR_FILE_TRANSFER), nullptr);
  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                    connectedSSID.c_str());

  if (!isApMode) {
    renderWifiIndicator(metrics.topPadding + metrics.headerHeight);
  }

  int startY = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing * 2;
  int height10 = renderer.getLineHeight(UI_10_FONT_ID);
  if (isApMode) {
    // AP mode display
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, startY, tr(STR_CONNECT_WIFI_HINT), true,
                      EpdFontFamily::BOLD);
    startY += height10 + metrics.verticalSpacing * 2;

    // Show QR code for Wifi
    const std::string wifiConfig = privateApMode
                                       ? std::string("WIFI:T:WPA;S:") + connectedSSID + ";P:" + privateApPassword + ";;"
                                       : std::string("WIFI:S:") + connectedSSID + ";;";
    const Rect qrBoundsWifi(metrics.contentSidePadding, startY, QR_CODE_WIDTH, QR_CODE_HEIGHT);
    QrUtils::drawQrCode(renderer, qrBoundsWifi, wifiConfig);

    // Show network name
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding + QR_CODE_WIDTH + metrics.verticalSpacing, startY + 80,
                      connectedSSID.c_str());

    startY += QR_CODE_HEIGHT + 2 * metrics.verticalSpacing;

    // Show primary URL (hostname)
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, startY, tr(STR_OPEN_URL_HINT), true,
                      EpdFontFamily::BOLD);
    startY += height10 + metrics.verticalSpacing * 2;

    std::string hostnameUrl = std::string("http://") + AP_HOSTNAME + ".local/";
    std::string ipUrl = "http://" + connectedIP + "/";
    const std::string& primaryUrl = gpio.deviceIsX3() ? ipUrl : hostnameUrl;

    // Show QR code for URL
    const Rect qrBoundsUrl(metrics.contentSidePadding, startY, QR_CODE_WIDTH, QR_CODE_HEIGHT);
    QrUtils::drawQrCode(renderer, qrBoundsUrl, primaryUrl);

    // Show IP address as fallback
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding + QR_CODE_WIDTH + metrics.verticalSpacing, startY + 80,
                      primaryUrl.c_str());
    if (!gpio.deviceIsX3()) {
      const std::string fallbackUrl = std::string(tr(STR_OR_HTTP_PREFIX)) + connectedIP + "/";
      renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding + QR_CODE_WIDTH + metrics.verticalSpacing,
                        startY + 100, fallbackUrl.c_str());
    }
  } else {
    startY += metrics.verticalSpacing * 2;

    // STA mode display (original behavior)
    // std::string ipInfo = "IP Address: " + connectedIP;
    renderer.drawCenteredText(UI_10_FONT_ID, startY, tr(STR_OPEN_URL_HINT), true, EpdFontFamily::BOLD);
    startY += height10;
    renderer.drawCenteredText(UI_10_FONT_ID, startY, tr(STR_SCAN_QR_HINT), true, EpdFontFamily::BOLD);
    startY += height10 + metrics.verticalSpacing * 2;

    // Show QR code for URL
    std::string webInfo = "http://" + connectedIP + "/";
    const Rect qrBounds((pageWidth - QR_CODE_WIDTH) / 2, startY, QR_CODE_WIDTH, QR_CODE_HEIGHT);
    QrUtils::drawQrCode(renderer, qrBounds, webInfo);
    startY += QR_CODE_HEIGHT + metrics.verticalSpacing * 2;

    // Show web server URL prominently
    renderer.drawCenteredText(UI_10_FONT_ID, startY, webInfo.c_str(), true);
    startY += height10 + 5;

    // X4 retains mDNS; X3 intentionally spends that heap on the transfer.
    if (!gpio.deviceIsX3()) {
      std::string hostnameUrl = std::string(tr(STR_OR_HTTP_PREFIX)) + AP_HOSTNAME + ".local/";
      renderer.drawCenteredText(SMALL_FONT_ID, startY, hostnameUrl.c_str(), true);
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_EXIT), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void CrossPointWebServerActivity::renderNearbySync() const {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_NEARBY_SYNC), nullptr);

  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 4;
  if (state == WebServerActivityState::NEARBY_STARTING) {
    renderer.drawCenteredText(
        UI_10_FONT_ID, y,
        nearbyCancelRequested.load(std::memory_order_acquire) ? tr(STR_NEARBY_CANCELLING) : tr(STR_NEARBY_STARTING),
        true, EpdFontFamily::BOLD);
  } else if (state == WebServerActivityState::NEARBY_HANDOFF) {
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_NEARBY_PREPARING), true, EpdFontFamily::BOLD);
  } else {
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_NEARBY_OPEN_APP), true, EpdFontFamily::BOLD);
    y += lineHeight + metrics.verticalSpacing * 3;
    renderer.drawCenteredText(UI_10_FONT_ID, y, nearbySync.advertisedName(), true);
    y += lineHeight + metrics.verticalSpacing * 4;

    if (!nearbySync.isAuthenticated()) {
      renderer.drawCenteredText(SMALL_FONT_ID, y, tr(STR_NEARBY_PAIR_CODE), true);
      y += renderer.getLineHeight(SMALL_FONT_ID) + metrics.verticalSpacing;
      char passkey[7];
      snprintf(passkey, sizeof(passkey), "%06lu", static_cast<unsigned long>(nearbySync.passkey()));
      renderer.drawCenteredText(UI_12_FONT_ID, y, passkey, true, EpdFontFamily::BOLD);
      y += renderer.getLineHeight(UI_12_FONT_ID) + metrics.verticalSpacing * 3;
      renderer.drawCenteredText(SMALL_FONT_ID, y, tr(STR_NEARBY_WAITING), true);
    } else {
      renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_NEARBY_CONNECTED), true, EpdFontFamily::BOLD);
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_EXIT), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void CrossPointWebServerActivity::renderWifiIndicator(int subHeaderTop) const {
  constexpr int BAR_COUNT = 4;
  constexpr int BAR_WIDTH = 4;
  constexpr int BAR_GAP = 2;
  constexpr int ICON_HEIGHT = 14;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int iconWidth = BAR_COUNT * BAR_WIDTH + (BAR_COUNT - 1) * BAR_GAP;
  const int iconRight = renderer.getScreenWidth() - metrics.contentSidePadding;
  const int iconLeft = iconRight - iconWidth;
  const int iconBottom = subHeaderTop + metrics.tabBarHeight - metrics.verticalSpacing;

  const bool wifiUp = (WiFi.status() == WL_CONNECTED) && (consecutiveDisconnects == 0);
  if (wifiUp) {
    for (int i = 0; i < BAR_COUNT; i++) {
      const int barHeight = (i + 1) * ICON_HEIGHT / BAR_COUNT;
      const int x = iconLeft + i * (BAR_WIDTH + BAR_GAP);
      const int y = iconBottom - barHeight;
      if (i < lastWifiBars) {
        renderer.fillRect(x, y, BAR_WIDTH, barHeight, true);
      } else {
        renderer.drawRect(x, y, BAR_WIDTH, barHeight, true);
      }
    }
  } else {
    const int xSize = ICON_HEIGHT;
    const int x0 = iconRight - xSize;
    const int y0 = iconBottom - xSize;
    renderer.drawLine(x0, y0, x0 + xSize, y0 + xSize, 2, true);
    renderer.drawLine(x0, y0 + xSize, x0 + xSize, y0, 2, true);
  }
}
