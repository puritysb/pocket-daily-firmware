#pragma once

#include <HalStorage.h>
#include <NetworkClient.h>
#include <NetworkServer.h>
#include <NetworkUdp.h>
#include <WebServer.h>
#include <WebSocketsServer.h>

#include <cstdint>
#include <memory>
#include <string>

enum class CrossPointWebServerProfile : uint8_t {
  FULL,
  FILE_TRANSFER,
  POCKET_SYNC,
};

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

  WsUploadStatus getWsUploadStatus() const;

  // Get the port number
  uint16_t getPort() const { return port; }

 private:
  std::unique_ptr<WebServer> server = nullptr;
  std::unique_ptr<WebSocketsServer> wsServer = nullptr;
  bool running = false;
  bool apMode = false;  // true when running in AP mode, false for STA mode
  CrossPointWebServerProfile profile;
  uint16_t port = 80;
  uint16_t wsPort = 81;  // WebSocket port
  static constexpr uint16_t pocketUploadPort = 82;
  NetworkUDP udp;
  bool udpActive = false;

  // Pocket's upload data plane uses one long-lived TCP connection. Arduino's
  // WebServer closes every HTTP request, which leaves hundreds of lwIP sockets
  // in TIME_WAIT for a multi-megabyte firmware upload on the no-PSRAM X3. This
  // listener is deliberately tiny and is serviced incrementally from the
  // activity loop so physical buttons remain responsive during SD writes.
  enum class PocketUploadPhase : uint8_t { IDLE, HEADER, DATA, REPLIED };
  std::unique_ptr<NetworkServer> pocketUploadServer = nullptr;
  NetworkClient pocketUploadClient;
  PocketUploadPhase pocketUploadPhase = PocketUploadPhase::IDLE;
  HalFile pocketUploadFile;
  char pocketUploadHeader[320] = {};
  size_t pocketUploadHeaderLength = 0;
  String pocketUploadFullPath;
  size_t pocketUploadExpected = 0;
  size_t pocketUploadReceived = 0;
  uint32_t pocketUploadCrc32 = 0xFFFFFFFFU;
  unsigned long pocketUploadLastActivity = 0;

  void handlePocketUploadStream();
  bool beginPocketUploadFromHeader();
  void finishPocketUploadStream();
  void failPocketUploadStream(const char* message, bool removePartial = true);
  void resetPocketUploadStream(bool removePartial);

  // WebSocket upload state
  void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length);
  static void wsEventCallback(uint8_t num, WStype_t type, uint8_t* payload, size_t length);
  void abortWsUpload(const char* tag);

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
