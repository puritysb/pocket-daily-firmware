#pragma once

#include <HalStorage.h>
#include <NetworkUdp.h>
#include <WebServer.h>
#include <WebSocketsServer.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "pocket_daily/direct_session.h"
#include "pocket_daily/web/PocketWebServices.h"

// Pocket-owned profile enum (route/listener/watchdog gating lives with the
// pocket web modules); alias keeps every existing call site unchanged.
using CrossPointWebServerProfile = PocketDaily::Web::Profile;

// Structure to hold file information
struct FileInfo {
  String name;
  size_t size;
  bool isEpub;
  bool isDirectory;
};

class CrossPointWebServer {
 public:
  struct WsUploadStatus {
    bool inProgress = false;
    size_t received = 0;
    size_t total = 0;
    std::string filename;
    std::string lastCompleteName;
    size_t lastCompleteSize = 0;
    unsigned long lastCompleteAt = 0;
  };

  // Used by POST upload handler
  struct UploadState {
    HalFile file;
    String fileName;
    String path = "/";
    size_t size = 0;
    uint32_t crc32 = 0xFFFFFFFFU;
    bool success = false;
    String error = "";
    bool chunked = false;
    size_t chunkStart = 0;

    // Upload write buffer batches small writes into SD card operations.
    // The HTTP layer already delivers multipart uploads in roughly 1.4 KiB
    // pieces. A second 4 KiB allocation repeatedly failed after Wi-Fi startup
    // on the no-PSRAM X3. Keep SD writes bounded without requiring that large
    // contiguous heap block.
    static constexpr size_t UPLOAD_BUFFER_SIZE = 1024;
    std::unique_ptr<uint8_t[]> buffer;
    size_t bufferPos = 0;
  } upload;

  explicit CrossPointWebServer(CrossPointWebServerProfile profile = CrossPointWebServerProfile::FULL);
  ~CrossPointWebServer();

  // Start the web server (call after WiFi is connected)
  void begin();

  // Stop the web server
  void stop();

  // Call this periodically to handle client requests
  void handleClient();

  // Check if server is running
  bool isRunning() const { return running; }

  // Last accepted HTTP request or Pocket upload-stream activity. The private
  // AP activity uses this timestamp to expire idle sessions without cutting
  // off a transfer that is still making progress.
  unsigned long lastClientActivityAt() const { return clientActivityAt; }

  // Live Studio LS-3: ask the hosting activity for one render pass after a
  // UI pack apply (and by the dev-only render trigger).
  void requestRepaint();
  bool consumeRepaintRequest();

  bool shouldEndSession() const {
    return PocketDaily::DirectSession::shouldEnd(sessionEndRequested, sessionEndRequestedAt, millis());
  }

  WsUploadStatus getWsUploadStatus() const;

  // Get the port number
  uint16_t getPort() const { return port; }

 private:
  std::unique_ptr<WebServer> server = nullptr;
  std::unique_ptr<WebSocketsServer> wsServer = nullptr;
  bool sessionEndRequested = false;
  unsigned long sessionEndRequestedAt = 0;
  bool running = false;
  bool apMode = false;  // true when running in AP mode, false for STA mode
  CrossPointWebServerProfile profile;
  uint16_t port = 80;
  uint16_t wsPort = 81;  // WebSocket port
  NetworkUDP udp;
  bool udpActive = false;

  // Pocket web services (SEAM.md): the upload-stream data plane and the host
  // reach-in bundle it is wired with. These members and their one-line hooks
  // below are the only Pocket footprint this inherited file keeps.
  PocketDaily::Web::UploadStreamServer pocketStream;
  PocketDaily::Web::Host pocketHost;
  mutable unsigned long clientActivityAt = 0;

  void noteClientActivity() const;
  void wirePocketHost();

  // WebSocket upload state
  void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length);
  static void wsEventCallback(uint8_t num, WStype_t type, uint8_t* payload, size_t length);
  void abortWsUpload(const char* tag);

  // Live Studio v1 (`docs/live-studio-v1.md`): the WebSocket doubles as an
  // event push channel on STA when heap allows. `liveStudioPush` records
  // that the listener can serve events; the subscription, change signature,
  // and send pacing are per-connection state kept out of the wire module.
  bool liveStudioPush = false;
  bool liveStudioSubscribed = false;
  bool liveStudioClientAttached = false;
  uint8_t liveStudioClientNum = 255;
  uint32_t liveStudioLastSendMs = 0;
  uint32_t liveStudioLastCheckMs = 0;
  uint32_t liveStudioLastSignatureMs = 0;
  char liveStudioSignature[128] = {};
  mutable std::atomic<bool> repaintRequested{false};
  char activePackName[33] = {};
  char activePackVersion[17] = {};
  // Transfer focus: uploads own the DMA pool (the heap map showed it
  // bottoming 3.5 KB from empty at an 8.7 KB settle); the WS listener's
  // buffers are torn down for the duration and rebuilt afterwards.
  void startLiveListener();
  void suspendLiveListener();
  void resumeLiveListener();
  void beginTransferFocus();
  void endTransferFocus();
  bool liveListenerSuspended = false;

  String buildStatusJson() const;
  void sendLiveStudioLine(const char* line);
  void pushLiveStudioStatusIfChanged();
  void handlePocketScreenLive() const;
#ifdef ENABLE_DEV_REMOTE_FLASH
  // Developer builds only (`env:default`): network-reachable flash of the
  // staged /update.bin. Never compiled into release builds.
  void handleDevRemoteFlash();
#endif
  void handleUiPackList() const;
  void handleUiPackApply();

  // File scanning
  void scanFiles(const char* path, const std::function<void(FileInfo)>& callback) const;
  String formatFileSize(size_t bytes) const;
  bool isEpubFile(const String& filename) const;

  // Request handlers
  void handleRoot() const;
  void handleJszip() const;
  void handleNotFound() const;
  void handleStatus() const;
  void handleCrashReport() const;
  void handlePocketScreenPreview() const;
  void handleFileList() const;
  void handleFileListData() const;
  void handleDownload() const;
  void handleUpload(UploadState& state) const;
  void handleUploadPost(UploadState& state) const;
  void handleCommitUpload();
  void handleCreateFolder() const;
  void handleRename() const;
  void handleMove() const;
  void handleDelete() const;

  // Settings handlers
  void handleSettingsPage() const;
  void handleGetSettings() const;
  void handlePostSettings();
  void handleGetPocketPreferences() const;
  void handlePostPocketPreferences();

  // Font management handlers
  void handleFontsPage() const;
  void handleFontList() const;
  void handleFontUpload();
  void handleFontUploadData();
  void handleFontDelete();

  // Font upload state
  struct FontUploadState {
    HalFile file;
    std::string familyName;
    std::string filePath;
    bool valid = false;
    bool magicChecked = false;
    size_t bytesWritten = 0;
    static constexpr size_t BUFFER_SIZE = 4096;
    std::unique_ptr<uint8_t[]> buffer;
    size_t bufferPos = 0;
  } fontUpload;

  // OPDS server handlers
  void handleGetOpdsServers() const;
  void handlePostOpdsServer();
  void handleDeleteOpdsServer();

  // Wi-Fi credential handlers
  void handleGetWifiNetworks() const;
  void handlePostWifiNetwork();
  void handleDeleteWifiNetwork();
};
