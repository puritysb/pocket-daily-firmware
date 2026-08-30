#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include "NetworkModeSelectionActivity.h"
#include "activities/Activity.h"
#include "network/CrossPointWebServer.h"
#include "pocket_daily/nearby_sync/NearbySyncService.h"

// Web server activity states
enum class WebServerActivityState {
  MODE_SELECTION,   // Choosing between Join Network and Create Hotspot
  WIFI_SELECTION,   // WiFi selection subactivity is active (for Join Network mode)
  AP_STARTING,      // Starting Access Point mode
  SERVER_STARTING,  // Reclaiming heap and constructing the transfer server
  SERVER_RUNNING,   // Web server is running and handling requests
  NEARBY_STARTING,  // Initializing the BLE control plane
  NEARBY_READY,     // Advertising or paired with the Pocket app
  NEARBY_HANDOFF,   // BLE lease delivered; waiting before starting private AP
  SHUTTING_DOWN     // Shutting down server and radios
};

enum class WebServerLaunchMode { FILE_TRANSFER, POCKET_NEARBY_SYNC };

/**
 * CrossPointWebServerActivity is the entry point for file transfer functionality.
 * It:
 * - First presents a choice between "Join a Network" (STA), "Connect to Calibre", and "Create Hotspot" (AP)
 * - For STA mode: Launches WifiSelectionActivity to connect to an existing network
 * - For AP mode: Creates an Access Point that clients can connect to
 * - Starts the CrossPointWebServer when connected
 * - Handles client requests in its loop() function
 * - Cleans up the server and shuts down WiFi on exit
 */
class CrossPointWebServerActivity final : public Activity {
  WebServerLaunchMode launchMode;
  WebServerActivityState state = WebServerActivityState::MODE_SELECTION;

  // Network mode
  NetworkMode networkMode = NetworkMode::JOIN_NETWORK;
  bool isApMode = false;
  bool privateApMode = false;
  char privateApSsid[20] = {};
  char privateApPassword[16] = {};
  unsigned long nearbyHandoffAt = 0;
  unsigned long privateApStartedAt = 0;
  bool lastNearbyAuthenticated = false;

  Pocket::NearbySync::Service nearbySync;
  enum class NearbyStartResult : uint8_t { IDLE, RUNNING, SUCCEEDED, FAILED };
  std::atomic<NearbyStartResult> nearbyStartResult{NearbyStartResult::IDLE};
  std::atomic<bool> nearbyCancelRequested{false};
  TaskHandle_t nearbyStartTask = nullptr;
  const char* nearbyModel = "X4";

  // Web server - owned by this activity
  std::unique_ptr<CrossPointWebServer> webServer;

  // Server status
  std::string connectedIP;
  std::string connectedSSID;  // For STA mode: network name, For AP mode: AP name

  // Performance monitoring
  unsigned long lastHandleClientTime = 0;

  // Sustained WiFi-loss tracking; abandon only after WIFI_ABANDON_MS.
  int consecutiveDisconnects = 0;
  unsigned long firstDisconnectAt = 0;
  static constexpr unsigned long WIFI_ABANDON_MS = 5UL * 60UL * 1000UL;

  // Cached signal-strength bracket (0..4) for the WiFi indicator.
  int lastWifiBars = 0;

  void renderServerRunning() const;
  void renderWifiIndicator(int subHeaderTop) const;

  void onNetworkModeSelected(NetworkMode mode);
  void onWifiSelectionComplete(bool connected);
  void startAccessPoint();
  void startWebServer();
  void startNearbySync();
  void handleNearbyStartup();
  static void nearbyStartTaskTrampoline(void* context);
  void handleNearbySync();
  void renderNearbySync() const;
  void returnToLaunchOrigin();

 public:
  explicit CrossPointWebServerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                       WebServerLaunchMode launchMode = WebServerLaunchMode::FILE_TRANSFER)
      : Activity(launchMode == WebServerLaunchMode::POCKET_NEARBY_SYNC ? "PocketNearbySync" : "CrossPointWebServer",
                 renderer, mappedInput),
        launchMode(launchMode) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return webServer && webServer->isRunning(); }
  bool preventAutoSleep() override {
    return state == WebServerActivityState::NEARBY_STARTING || state == WebServerActivityState::NEARBY_HANDOFF ||
           (webServer && webServer->isRunning()) || nearbySync.isRunning();
  }
};
