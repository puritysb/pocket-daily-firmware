#include "CrossPointWebServer.h"

#include <ArduinoJson.h>
#include <FsHelpers.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "FirmwareFlasher.h"
#include "FontInstaller.h"
#include "OpdsServerStore.h"
#include "SdCardFontSystem.h"
#include "SettingsList.h"
#include "WebDAVHandler.h"
#include "WifiCredentialStore.h"
#include "components/UITheme.h"
#include "html/FilesPageHtml.generated.h"
#include "html/FontsPageHtml.generated.h"
#include "html/HomePageHtml.generated.h"
#include "html/SettingsPageHtml.generated.h"
#include "html/js/jszip_minJs.generated.h"
#include "pocket_daily/PocketScreenPreview.h"
#include "pocket_daily/live_studio/DevTrace.h"
#include "pocket_daily/live_studio/HeapMap.h"
#include "pocket_daily/live_studio/LiveFrameCapture.h"
#include "pocket_daily/live_studio/LiveStudioEvents.h"
#include "pocket_daily/live_studio/NetHealth.h"
#include "pocket_daily/live_studio/StackReport.h"
#include "pocket_daily/live_studio/UiPackStore.h"
#ifdef ENABLE_DEV_REMOTE_FLASH
#include "pocket_daily/product_identity.h"
#endif
#include "pocket_daily/upload_stream_protocol.h"
#include "util/BookCacheUtils.h"

namespace {
// Folders/files to hide from the web interface file browser
// Note: Items starting with "." are automatically hidden
constexpr const char* HIDDEN_ITEMS[] = {"System Volume Information", "XTCache"};
constexpr uint16_t UDP_PORTS[] = {54982, 48123, 39001, 44044, 59678};
constexpr uint16_t LOCAL_UDP_PORT = 8134;
// Keep diagnostic responses below the Pocket private AP's scarce contiguous
// heap and return to the global loop between every piece. The previous 512 B
// write loop could block inside lwIP long enough to trip the task watchdog.
// Each diagnostic request reads one chunk into the shared static staging
// buffer and answers with one bounded socket write. A 53 KB X3 frame needs
// ~13 requests instead of ~52.
constexpr size_t CRASH_REPORT_CHUNK_BYTES = 1024;
constexpr size_t SCREEN_PREVIEW_CHUNK_BYTES = firmware_flash::STAGING_BUFFER_BYTES;
constexpr unsigned long UPLOAD_SOCKET_TIMEOUT_MS = 30 * 1000;
// A diagnostic chunk send blocks the loop task. Unlike an SD write it can
// hang forever if the peer vanishes, so the loop watchdog must stay armed;
// bounding the socket below the 5 s task-WDT window guarantees the send
// returns (completed or aborted) before the watchdog could fire.
constexpr unsigned long DIAGNOSTIC_SEND_TIMEOUT_MS = 3000;

// Static pointer for WebSocket callback (WebSocketsServer requires C-style callback)
CrossPointWebServer* wsInstance = nullptr;

// WebSocket upload state
HalFile wsUploadFile;
String wsUploadFileName;
String wsUploadPath;
size_t wsUploadSize = 0;
size_t wsUploadReceived = 0;
unsigned long wsUploadStartTime = 0;
bool wsUploadInProgress = false;
uint8_t wsUploadClientNum = 255;  // 255 = no active upload client
size_t wsLastProgressSent = 0;
String wsLastCompleteName;
size_t wsLastCompleteSize = 0;
unsigned long wsLastCompleteAt = 0;

String normalizeWebPath(const String& inputPath) {
  if (inputPath.isEmpty() || inputPath == "/") {
    return "/";
  }
  std::string normalized = FsHelpers::normalisePath(inputPath.c_str());
  String result = normalized.c_str();
  if (result.isEmpty()) {
    return "/";
  }
  if (!result.startsWith("/")) {
    result = "/" + result;
  }
  if (result.length() > 1 && result.endsWith("/")) {
    result = result.substring(0, result.length() - 1);
  }
  return result;
}

bool isProtectedItemName(const String& name) {
  if (name.startsWith(".")) {
    return true;
  }
  for (const auto* item : HIDDEN_ITEMS) {
    if (name.equals(item)) {
      return true;
    }
  }
  return false;
}

uint32_t updateCrc32(uint32_t crc, const uint8_t* data, size_t length) {
  return PocketDaily::UploadStream::updateCrc32(crc, data, length);
}

bool parseUnsignedDecimal(const String& text, size_t& value) {
  if (text.isEmpty()) return false;
  size_t parsed = 0;
  for (size_t i = 0; i < text.length(); ++i) {
    const char c = text[i];
    if (c < '0' || c > '9') return false;
    const size_t digit = static_cast<size_t>(c - '0');
    if (parsed > (SIZE_MAX - digit) / 10) return false;
    parsed = parsed * 10 + digit;
  }
  value = parsed;
  return true;
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
}  // namespace

// File listing page template - now using generated headers:
// - HomePageHtml (from html/HomePage.html)
// - FilesPageHeaderHtml (from html/FilesPageHeader.html)
// - FilesPageFooterHtml (from html/FilesPageFooter.html)
CrossPointWebServer::CrossPointWebServer(const CrossPointWebServerProfile profile) : profile(profile) {}

CrossPointWebServer::~CrossPointWebServer() { stop(); }

void CrossPointWebServer::begin() {
  if (running) {
    LOG_DBG("WEB", "Web server already running");
    return;
  }

  // Check if we have a valid network connection (either STA connected or AP mode)
  const wifi_mode_t wifiMode = WiFi.getMode();
  const bool isStaConnected = (wifiMode & WIFI_MODE_STA) && (WiFi.status() == WL_CONNECTED);
  const bool isInApMode = (wifiMode & WIFI_MODE_AP) && (WiFi.softAPgetStationNum() >= 0);  // AP is running

  if (!isStaConnected && !isInApMode) {
    LOG_DBG("WEB", "Cannot start webserver - no valid network (mode=%d, status=%d)", wifiMode, WiFi.status());
    return;
  }

  sessionEndRequested = false;

  // Store AP mode flag for later use (e.g., in handleStatus)
  apMode = isInApMode;

  LOG_DBG("WEB", "[MEM] Free heap before begin: %d bytes", ESP.getFreeHeap());
  LOG_DBG("WEB", "Network mode: %s", apMode ? "AP" : "STA");

  LOG_DBG("WEB", "Creating web server on port %d...", port);
  server = makeUniqueNoThrow<WebServer>(port);

  if (!server) {
    LOG_ERR("WEB", "Failed to create WebServer!");
    return;
  }

  // Disable WiFi sleep to improve responsiveness and prevent 'unreachable' errors.
  // This is critical for reliable web server operation on ESP32.
  WiFi.setSleep(false);
  // Default varies by ESP32 core version. The activity's loss-recovery loop
  // relies on driver retries during transient disconnects.
  WiFi.setAutoReconnect(true);

  // Note: WebServer class doesn't have setNoDelay() in the standard ESP32 library.
  // We rely on disabling WiFi sleep for responsiveness.

  LOG_DBG("WEB", "[MEM] Free heap after WebServer allocation: %d bytes", ESP.getFreeHeap());

  // Setup routes
  LOG_DBG("WEB", "Setting up routes...");
  server->on("/", HTTP_GET, [this] {
    noteClientActivity();
    handleRoot();
  });
  // A phone heartbeat must not hold an otherwise idle direct session forever.
  server->on("/api/status", HTTP_GET, [this] { handleStatus(); });
  if (profile == CrossPointWebServerProfile::POCKET_SYNC && apMode) {
    server->on("/api/pocket/v1/session/end", HTTP_POST, [this] {
      if (upload.file || pocketStream.transferActive()) {
        server->send(409, "application/json", "{\"error\":\"transfer active\"}");
        return;
      }
      server->send(200, "application/json", "{\"ended\":true}");
      sessionEndRequestedAt = millis();
      sessionEndRequested = true;
    });
  }
  // Diagnostics is available in every profile so a phone can retrieve the
  // previous panic after the reader has recovered, without exposing arbitrary
  // hidden files or requiring the SD card to be removed.
  server->on("/api/pocket/v1/crash-report", HTTP_GET, [this] {
    noteClientActivity();
    handleCrashReport();
  });

  // Upload endpoint with special handling for multipart form data
  server->on(
      "/upload", HTTP_POST,
      [this] {
        noteClientActivity();
        handleUploadPost(upload);
      },
      [this] {
        noteClientActivity();
        handleUpload(upload);
      });
  // Pocket clients upload to a unique hidden .part file, then ask the reader
  // to verify size + CRC and atomically publish it. A dropped phone or Wi-Fi
  // link therefore never turns a valid book or update.bin into a partial file.
  server->on("/api/pocket/v1/commit", HTTP_POST, [this] {
    noteClientActivity();
    handleCommitUpload();
  });

  // Pocket only needs four preferences. Avoid constructing and streaming the
  // full localized settings registry on the no-PSRAM private-AP path: that
  // response can consume the last contiguous heap immediately after Wi-Fi
  // starts and strand the X3 on its retained Hotspot Mode frame.
  if (profile == CrossPointWebServerProfile::POCKET_SYNC) {
    server->on("/api/pocket/v1/screen-preview", HTTP_GET, [this] {
      noteClientActivity();
      handlePocketScreenPreview();
    });
    server->on("/api/pocket/v1/preferences", HTTP_GET, [this] {
      noteClientActivity();
      handleGetPocketPreferences();
    });
    server->on("/api/pocket/v1/preferences", HTTP_POST, [this] {
      noteClientActivity();
      handlePostPocketPreferences();
    });
  } else {
    // Live Studio LS-2/LS-3: chunked fetch of the latest captured frame and
    // the UI-pack endpoints, served wherever the WS push listener can run
    // (STA profiles).
    server->on("/api/pocket/v1/screen-live", HTTP_GET, [this] {
      noteClientActivity();
      handlePocketScreenLive();
    });
    server->on("/api/pocket/v1/ui-packs", HTTP_GET, [this] {
      noteClientActivity();
      handleUiPackList();
    });
    server->on("/api/pocket/v1/ui-pack/apply", HTTP_POST, [this] {
      noteClientActivity();
      handleUiPackApply();
    });
    server->on("/settings", HTTP_GET, [this] { handleSettingsPage(); });
    server->on("/api/settings", HTTP_GET, [this] { handleGetSettings(); });
    server->on("/api/settings", HTTP_POST, [this] { handlePostSettings(); });
  }

  // Pocket Sync deliberately exposes only the routes used by the companion
  // app. The full browser/File Transfer surface remains available as a
  // separate profile without imposing WebDAV/WebSocket/discovery allocations
  // on the X3 private-AP path.
  if (profile != CrossPointWebServerProfile::POCKET_SYNC) {
    server->on("/files", HTTP_GET, [this] { handleFileList(); });
    server->on("/js/jszip.min.js", HTTP_GET, [this] { handleJszip(); });
    server->on("/api/files", HTTP_GET, [this] { handleFileListData(); });
    server->on("/download", HTTP_GET, [this] { handleDownload(); });
    server->on("/mkdir", HTTP_POST, [this] { handleCreateFolder(); });
    server->on("/rename", HTTP_POST, [this] { handleRename(); });
    server->on("/move", HTTP_POST, [this] { handleMove(); });
    server->on("/delete", HTTP_POST, [this] { handleDelete(); });

    // Font management endpoints
    server->on("/fonts", HTTP_GET, [this] { handleFontsPage(); });
    server->on("/api/fonts", HTTP_GET, [this] { handleFontList(); });
    server->on("/api/fonts/upload", HTTP_POST, [this] { handleFontUpload(); }, [this] { handleFontUploadData(); });
    server->on("/api/fonts/delete", HTTP_POST, [this] { handleFontDelete(); });

    // OPDS server endpoints
    server->on("/api/opds", HTTP_GET, [this] { handleGetOpdsServers(); });
    server->on("/api/opds", HTTP_POST, [this] { handlePostOpdsServer(); });
    server->on("/api/opds/delete", HTTP_POST, [this] { handleDeleteOpdsServer(); });

    // Wi-Fi credential endpoints
    server->on("/api/wifi", HTTP_GET, [this] { handleGetWifiNetworks(); });
    server->on("/api/wifi", HTTP_POST, [this] { handlePostWifiNetwork(); });
    server->on("/api/wifi/delete", HTTP_POST, [this] { handleDeleteWifiNetwork(); });
  }

  server->onNotFound([this] { handleNotFound(); });
#if POCKET_HEAP_MAP_ENABLED
  // HN-2 evidence part 2: per-task stack high-water marks. Safe API per task
  // (no scheduler suspension, unlike uxTaskGetSystemState which hung).
  server->on("/api/pocket/v1/dev/stack-report", HTTP_GET, [this] {
    noteClientActivity();
    char report[640];
    const size_t n = PocketDaily::StackReport::render(report, sizeof(report));
    server->send(200, "text/plain; charset=utf-8", n ? String(report) : "unavailable");
  });
  // HN-2 evidence: caps-only, fixed small response. The fuller map hung the
  // request on the loaded X3 (uxTaskGetSystemState suspends every task; the
  // chunked variant never answered). Three numbers per capability survive
  // any fragmentation.
  server->on("/api/pocket/v1/dev/heap-map", HTTP_GET, [this] {
    noteClientActivity();
    char map[256];
    const size_t n = PocketDaily::HeapMap::renderCaps(map, sizeof(map));
    server->send(200, "text/plain; charset=utf-8", n ? String(map) : "unavailable");
  });
#endif
#ifdef ENABLE_DEV_REMOTE_FLASH
  // Developer builds only: flash the staged /update.bin over the LAN so
  // iteration does not walk the on-device Settings menus. Absent from
  // gh_release builds by build flag, not by request filtering.
  server->on("/api/pocket/v1/dev/flash", HTTP_POST, [this] {
    noteClientActivity();
    handleDevRemoteFlash();
  });
  // Developer builds only: force one repaint (live-frame debugging).
  server->on("/api/pocket/v1/dev/render", HTTP_POST, [this] {
    noteClientActivity();
    DEV_TRACE(PocketDaily::DevTrace::RENDER_REQ);
    requestRepaint();
    server->send(204, "text/plain", "");
  });

  // Developer builds only: network stability post-mortem log
  // (docs/network-stability-analysis.md). Readable after the radio died.
  server->on("/api/pocket/v1/dev/net-health", HTTP_GET, [this] {
    noteClientActivity();
    static char tail[4096];
    const size_t n = PocketDaily::NetHealth::tail(tail, sizeof(tail));
    server->send(200, "text/plain; charset=utf-8", n ? String(tail) : "no net-health log yet");
  });
  // Developer builds only: dump the live-studio trace ring (text lines
  // "ms tag aux"), oldest first. A gap between heartbeats is the stall.
  server->on("/api/pocket/v1/dev/live-debug", HTTP_GET, [this] {
    noteClientActivity();
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
  LOG_DBG("WEB", "[MEM] Free heap after route setup: %d bytes", ESP.getFreeHeap());

  if (profile == CrossPointWebServerProfile::FULL) {
    // Collect WebDAV headers and register handler
    const char* davHeaders[] = {"Depth", "Destination", "Overwrite", "If", "Lock-Token", "Timeout"};
    server->collectHeaders(davHeaders, 6);
    auto* webDav = new (std::nothrow) WebDAVHandler();
    if (webDav) {
      server->addHandler(webDav);  // Deleted by WebServer when the server is stopped.
      LOG_DBG("WEB", "WebDAV handler initialized");
    } else {
      LOG_ERR("WEB", "Could not allocate WebDAV handler");
    }
  }

  server->begin();

  // X3/X4 Pocket uploads use one persistent, allocation-light connection.
  // Keep the legacy HTTP upload route for browser clients and old companions.
  wirePocketHost();
  pocketStream.begin(
      PocketDaily::Web::UploadStreamServer::Config{82, profile == CrossPointWebServerProfile::POCKET_SYNC},
      &pocketHost);

  // The generic browser/File Transfer surface keeps its legacy WebSocket
  // upload surface. On STA the same listener additionally serves Live Studio
  // v1 event push when heap allows (`docs/live-studio-v1.md`); the private
  // AP stays poll-only to preserve contiguous heap on X3.
  wireLiveStudioHost();
  liveStudio.begin(PocketDaily::Web::LiveStudioService::Config{apMode, profile, wsPort, &pocketStream},
                   &liveStudioHost);

  if (profile == CrossPointWebServerProfile::FULL) {
    udpActive = udp.begin(LOCAL_UDP_PORT);
    LOG_DBG("WEB", "Discovery UDP %s on port %d", udpActive ? "enabled" : "failed", LOCAL_UDP_PORT);
  }

  running = true;
  clientActivityAt = 0;

  LOG_DBG("WEB", "Web server started on port %d", port);
  // Show the correct IP based on network mode
  const String ipAddr = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  LOG_DBG("WEB", "Access at http://%s/", ipAddr.c_str());
  if (profile == CrossPointWebServerProfile::FULL || liveStudio.pushActive()) {
    LOG_DBG("WEB", "WebSocket at ws://%s:%d/ (liveStudioPush=%d)", ipAddr.c_str(), wsPort,
            liveStudio.pushActive());
  }
  LOG_DBG("WEB", "[MEM] Free heap after server.begin(): %d bytes", ESP.getFreeHeap());
}

void CrossPointWebServer::abortWsUpload(const char* tag) {
  // Explicit close() required: file-scope global persists beyond function scope
  wsUploadFile.close();
  String filePath = wsUploadPath;
  if (!filePath.endsWith("/")) filePath += "/";
  filePath += wsUploadFileName;
  if (Storage.remove(filePath.c_str())) {
    LOG_DBG(tag, "Deleted incomplete upload: %s", filePath.c_str());
  } else {
    LOG_DBG(tag, "Failed to delete incomplete upload: %s", filePath.c_str());
  }
  wsUploadInProgress = false;
  wsUploadClientNum = 255;
  wsLastProgressSent = 0;
}

void CrossPointWebServer::stop() {
  if (!running || !server) {
    LOG_DBG("WEB", "stop() called but already stopped (running=%d, server=%p)", running, server.get());
    return;
  }

  LOG_DBG("WEB", "STOP INITIATED - setting running=false first");
  running = false;  // Set this FIRST to prevent handleClient from using server

  LOG_DBG("WEB", "[MEM] Free heap before stop: %d bytes", ESP.getFreeHeap());

  pocketStream.stop();

  // Close any in-progress WebSocket upload and remove partial file
  if (wsUploadInProgress && wsUploadFile) {
    abortWsUpload("WEB");
  }

  // Stop WebSocket server
  if (wsServer) {
    LOG_DBG("WEB", "Stopping WebSocket server...");
    liveStudio.onServerStopping();
  }

  if (udpActive) {
    udp.stop();
    udpActive = false;
  }

  // Brief delay to allow any in-flight handleClient() calls to complete
  delay(20);

  server->stop();
  LOG_DBG("WEB", "[MEM] Free heap after server->stop(): %d bytes", ESP.getFreeHeap());

  // Brief delay before deletion
  delay(10);

  server.reset();
  LOG_DBG("WEB", "Web server stopped and deleted");
  LOG_DBG("WEB", "[MEM] Free heap after delete server: %d bytes", ESP.getFreeHeap());

  // Note: Static upload variables (uploadFileName, uploadPath, uploadError) are declared
  // later in the file and will be cleared when they go out of scope or on next upload
  LOG_DBG("WEB", "[MEM] Free heap final: %d bytes", ESP.getFreeHeap());
}

void CrossPointWebServer::handleClient() {
  static unsigned long lastDebugPrint = 0;
  static unsigned long lastTraceHeartbeat = 0;
#ifdef ENABLE_DEV_REMOTE_FLASH
  // Dev trace heartbeat: a missing heartbeat in the dump is the stall window.
  if (millis() - lastTraceHeartbeat >= 1000) {
    lastTraceHeartbeat = millis();
    DEV_TRACE(PocketDaily::DevTrace::HEARTBEAT);
  }
#endif

  // Check running flag FIRST before accessing server
  if (!running) {
    return;
  }

  // Double-check server pointer is valid
  if (!server) {
    LOG_DBG("WEB", "WARNING: handleClient called with null server!");
    return;
  }

  // Print debug every 10 seconds to confirm handleClient is being called
  if (millis() - lastDebugPrint > 10000) {
    LOG_DBG("WEB", "handleClient active, server running on port %d", port);
    lastDebugPrint = millis();
  }

  server->handleClient();

  pocketStream.service();

  // Handle WebSocket events
  if (wsServer) {
    wsServer->loop();
    liveStudio.tick();
  }

  // Respond to discovery broadcasts
  if (udpActive) {
    int packetSize = udp.parsePacket();
    if (packetSize > 0) {
      char buffer[16];
      int len = udp.read(buffer, sizeof(buffer) - 1);
      if (len > 0) {
        buffer[len] = '\0';
        if (strcmp(buffer, "hello") == 0) {
          String hostname = WiFi.getHostname();
          if (hostname.isEmpty()) {
            hostname = "crosspoint";
          }
          String message = "crosspoint (on " + hostname + ");" + String(wsPort);
          udp.beginPacket(udp.remoteIP(), udp.remotePort());
          udp.write(reinterpret_cast<const uint8_t*>(message.c_str()), message.length());
          udp.endPacket();
        }
      }
    }
  }
}

void CrossPointWebServer::noteClientActivity() const { clientActivityAt = millis(); }

void CrossPointWebServer::beginTransferFocus() { liveStudio.beginTransferFocus(); }
void CrossPointWebServer::endTransferFocus() { liveStudio.endTransferFocus(); }

// Pocket seam (SEAM.md): captureless lambdas so the pocket web modules can
// reach this server without including or linking back into it.
void CrossPointWebServer::wirePocketHost() {
  pocketHost.self = this;
  pocketHost.noteClientActivity = [](void* self) { static_cast<CrossPointWebServer*>(self)->noteClientActivity(); };
  pocketHost.httpUploadBusy = [](void* self) {
    return static_cast<bool>(static_cast<CrossPointWebServer*>(self)->upload.file);
  };
  pocketHost.releaseHttpUploadBuffer = [](void* self) {
    static_cast<CrossPointWebServer*>(self)->upload.buffer.reset();
  };
  pocketHost.beginTransferFocus = [](void* self) { static_cast<CrossPointWebServer*>(self)->beginTransferFocus(); };
  pocketHost.endTransferFocus = [](void* self) { static_cast<CrossPointWebServer*>(self)->endTransferFocus(); };
  pocketHost.requestRepaint = [](void* self) { static_cast<CrossPointWebServer*>(self)->requestRepaint(); };
  pocketHost.normalizeWebPath = [](void*, const String& path) { return normalizeWebPath(path); };
  pocketHost.isProtectedItemName = [](void*, const String& name) { return isProtectedItemName(name); };
}

// Pocket seam (SEAM.md): the Live Studio service drives the host-owned
// WebSocketsServer slot; wire/unwire keep the file-static wsInstance
// trampoline and the legacy upload grammar's event callback in this file.
void CrossPointWebServer::wireLiveStudioHost() {
  liveStudioHost.self = this;
  liveStudioHost.wsSlot = &wsServer;
  liveStudioHost.wireListener = [](void* self, WebSocketsServer& server) {
    wsInstance = static_cast<CrossPointWebServer*>(self);
    server.begin();
    server.onEvent(CrossPointWebServer::wsEventCallback);
  };
  liveStudioHost.unwireListener = [](void*) {
    if (wsInstance) wsInstance = nullptr;
  };
  liveStudioHost.statusInputs = [](void* self) { return static_cast<CrossPointWebServer*>(self)->statusInputs(); };
  liveStudioHost.legacyUploadBusy = [](void*) { return wsUploadInProgress; };
}

CrossPointWebServer::WsUploadStatus CrossPointWebServer::getWsUploadStatus() const {
  WsUploadStatus status;
  status.inProgress = wsUploadInProgress;
  status.received = wsUploadReceived;
  status.total = wsUploadSize;
  status.filename = wsUploadFileName.c_str();
  status.lastCompleteName = wsLastCompleteName.c_str();
  status.lastCompleteSize = wsLastCompleteSize;
  status.lastCompleteAt = wsLastCompleteAt;
  return status;
}

static void sendHtmlContent(WebServer* server, const char* data, size_t len) {
  server->sendHeader("Content-Encoding", "gzip");
  server->send_P(200, "text/html", data, len);
}

void CrossPointWebServer::handleRoot() const {
  sendHtmlContent(server.get(), HomePageHtml, sizeof(HomePageHtml));
  LOG_DBG("WEB", "Served root page");
}

void CrossPointWebServer::handleJszip() const {
  server->sendHeader("Content-Encoding", "gzip");
  server->send_P(200, "application/javascript", jszip_minJs, jszip_minJsCompressedSize);
  LOG_DBG("WEB", "Served jszip.min.js");
}

void CrossPointWebServer::handleNotFound() const {
  String message = "404 Not Found\n\n";
  message += "URI: " + server->uri() + "\n";
  server->send(404, "text/plain", message);
}

// Pocket seam (SEAM.md): marshal host state into the value snapshot the
// pocket status module builds /api/status from.
PocketDaily::Web::StatusInputs CrossPointWebServer::statusInputs() const {
  PocketDaily::Web::StatusInputs in;
  in.apMode = apMode;
  in.profile = profile;
  in.stream = &pocketStream;
  in.live.push = liveStudio.pushActive();
  in.live.suspended = liveStudio.listenerSuspended();
  in.live.wsPort = wsPort;
  strlcpy(in.live.activePackName, liveStudio.activePackName(), sizeof(in.live.activePackName));
  strlcpy(in.live.activePackVersion, liveStudio.activePackVersion(), sizeof(in.live.activePackVersion));
  return in;
}

void CrossPointWebServer::handleStatus() const {
  server->send(200, "application/json", PocketDaily::Web::buildStatusJson(statusInputs()));
}

// LS-2 live frame fetch: same chunked octet-stream contract as the one-shot
// screen preview, but reading the latest captured frame. Single slot — the
// companion associates the fetched bytes with the newest `frame` event.
// The fetch gate sits below the diagnostics floor on purpose: a 4 KiB shared
// -buffer read plus one bounded send is far lighter than the crash report,
// and an X3 in STA File Transfer idles within a few hundred bytes of the
// 10 KiB diagnostics floor, which starved multi-chunk fetches in testing.
constexpr uint32_t LIVE_FETCH_MIN_FREE_HEAP = 6U * 1024U;
void CrossPointWebServer::handlePocketScreenLive() const {
  HalSystem::setCrashBreadcrumb("nearby:screen-live");
  if (ESP.getFreeHeap() < LIVE_FETCH_MIN_FREE_HEAP) {
    server->send(503, "text/plain", "Reader memory is too low for the live frame right now");
    return;
  }
  HalFile frame = Storage.open(PocketDaily::LiveFrameCapture::LIVE_FRAME_PATH);
  if (!frame || frame.isDirectory()) {
    if (frame) frame.close();
    server->send(404, "text/plain", "No live frame captured yet");
    return;
  }
  if (!server->hasArg("offset")) {
    frame.close();
    server->send(400, "text/plain", "Missing live frame offset");
    return;
  }
  const String offsetText = server->arg("offset");
  bool offsetValid = !offsetText.isEmpty();
  for (size_t i = 0; offsetValid && i < offsetText.length(); i++) {
    if (offsetText[i] < '0' || offsetText[i] > '9') offsetValid = false;
  }
  if (!offsetValid) {
    frame.close();
    server->send(400, "text/plain", "Invalid live frame offset");
    return;
  }
  const size_t frameSize = frame.size();
  const size_t offset = static_cast<size_t>(offsetText.toInt());
  if (offset >= frameSize || !frame.seek(offset)) {
    frame.close();
    server->send(416, "text/plain", "Live frame offset out of range");
    return;
  }
  uint8_t* body = firmware_flash::sharedStagingBuffer();
  const size_t requested = std::min(frameSize - offset, SCREEN_PREVIEW_CHUNK_BYTES);
  const int count = frame.read(body, requested);
  frame.close();
  if (count <= 0) {
    server->send(500, "text/plain", "Could not read live frame chunk");
    return;
  }
  server->client().setTimeout(DIAGNOSTIC_SEND_TIMEOUT_MS);
  feedLoopWDT();
  server->setContentLength(static_cast<size_t>(count));
  server->sendHeader("Cache-Control", "no-store");
  server->send(200, "application/octet-stream", "");
  server->sendContent(reinterpret_cast<const char*>(body), static_cast<size_t>(count));
  feedLoopWDT();
}

void CrossPointWebServer::requestRepaint() { repaintRequested.store(true); }

bool CrossPointWebServer::consumeRepaintRequest() { return repaintRequested.exchange(false); }

void CrossPointWebServer::handleUiPackList() const {
  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("[");
  char line[160];
  bool first = true;
  scanFiles(PocketDaily::LiveStudio::UIPACK_DIR, [this, &line, &first](const FileInfo& info) mutable {
    if (info.isDirectory || !info.name.endsWith(".uipack")) return;
    // Strip the extension for the pack name the apply endpoint expects.
    const std::string name = std::string(info.name.c_str(), info.name.length() - 7);
    const bool active = name == liveStudio.activePackName();
    const int n = snprintf(line, sizeof(line), R"({"name":"%.32s","size":%u,"active":%s})", name.c_str(),
                           static_cast<unsigned>(info.size), active ? "true" : "false");
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(line)) return;
    if (!first) server->sendContent(",");
    first = false;
    server->sendContent(line);
  });
  server->sendContent("]");
  server->sendContent("");
}

// LS-3 apply: load + validate + layer over the current theme, persist the
// choice, and ask for a repaint so the live frame shows the result. An empty
// name reverts to the theme's own metrics.
void CrossPointWebServer::handleUiPackApply() {
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, server->arg("plain"));
  if (err) {
    server->send(400, "text/plain", "Invalid JSON");
    return;
  }
  const char* name = doc["name"] | "";
  if (name[0] == '\0') {
    PocketDaily::LiveStudio::ThemeOverride none{};
    UITheme::getInstance().applyPackMetrics(&none, 0);
    liveStudio.onPackCleared();
    requestRepaint();
    server->send(200, "application/json", "{\"applied\":false}");
    return;
  }
  PocketDaily::LiveStudio::UiPackInfo info;
  PocketDaily::LiveStudio::ThemeOverride* overrides = PocketDaily::LiveStudio::acquireOverrideBuffer();
  if (!overrides) {
    server->send(503, "text/plain", "Not enough memory to load the pack");
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
    server->send(code, "application/json", body);
    return;
  }
  UITheme::getInstance().applyPackMetrics(overrides, info.themeOverrideCount);
  PocketDaily::LiveStudio::releaseOverrideBuffer(overrides);
  liveStudio.onPackApplied(info.name, info.packVersion);
  requestRepaint();
  char body[128];
  snprintf(body, sizeof(body), "{\"applied\":true,\"name\":\"%.32s\",\"version\":\"%.16s\",\"overrides\":%u}",
           info.name, info.packVersion, static_cast<unsigned>(info.themeOverrideCount));
  server->send(200, "application/json", body);
}

#ifdef ENABLE_DEV_REMOTE_FLASH
// Developer builds only. Validates and flashes the staged /update.bin, then
// reboots; a marker makes the next boot land in the File Transfer menu where
// one Confirm rejoins the saved network. The 200 response is sent before
// flashing because the loop blocks for the erase/write and ends in a chip
// restart.
void CrossPointWebServer::handleDevRemoteFlash() {
  HalSystem::setCrashBreadcrumb("dev:remote-flash");
  // Allowed on the reader's own hotspot too: when the STA path is the thing
  // being repaired, the dedicated AP link is the delivery route (and it has
  // no router in the path). Still a dev-build-only endpoint.
  if (!Storage.exists("/update.bin")) {
    server->send(404, "text/plain", "No /update.bin staged on the reader");
    return;
  }
  const esp_partition_t* dest = esp_ota_get_next_update_partition(nullptr);
  if (!dest) {
    server->send(500, "text/plain", "No OTA partition available");
    return;
  }
  if (firmware_flash::validateImageFile("/update.bin", dest->size) != firmware_flash::Result::OK) {
    server->send(422, "text/plain", "Staged /update.bin failed image validation");
    return;
  }
  server->sendHeader("Connection", "close");
  server->send(200, "text/plain", "Flashing /update.bin (dev build). Reader reboots to File Transfer.");
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

void CrossPointWebServer::handleCrashReport() const {
  HalSystem::setCrashBreadcrumb("nearby:crash-chunk");
  if (!PocketDaily::Web::diagnosticsAffordable()) {
    server->send(503, "text/plain", "Reader memory is too low for diagnostics right now");
    return;
  }
  HalFile report = Storage.open("/crash_report.txt");
  if (!report || report.isDirectory()) {
    if (report) report.close();
    server->send(404, "text/plain", "No crash report recorded");
    return;
  }

  if (!server->hasArg("offset")) {
    report.close();
    server->send(400, "text/plain", "Missing crash report offset");
    return;
  }

  const String offsetText = server->arg("offset");
  if (offsetText.isEmpty()) {
    report.close();
    server->send(400, "text/plain", "Invalid crash report offset");
    return;
  }
  for (size_t i = 0; i < offsetText.length(); i++) {
    if (offsetText[i] < '0' || offsetText[i] > '9') {
      report.close();
      server->send(400, "text/plain", "Invalid crash report offset");
      return;
    }
  }

  const size_t reportSize = report.size();
  const size_t offset = static_cast<size_t>(offsetText.toInt());
  if (offset >= reportSize || !report.seek(offset)) {
    report.close();
    server->send(416, "text/plain", "Crash report offset out of range");
    return;
  }

  uint8_t* body = firmware_flash::sharedStagingBuffer();
  const size_t remaining = reportSize - offset;
  const size_t requested = std::min(remaining, CRASH_REPORT_CHUNK_BYTES);
  const int count = report.read(body, requested);
  report.close();
  if (count <= 0) {
    server->send(500, "text/plain", "Could not read crash report chunk");
    return;
  }

  server->client().setTimeout(DIAGNOSTIC_SEND_TIMEOUT_MS);
  feedLoopWDT();
  server->setContentLength(static_cast<size_t>(count));
  server->sendHeader("Cache-Control", "no-store");
  server->send(200, "text/plain; charset=utf-8", "");
  server->sendContent(reinterpret_cast<const char*>(body), static_cast<size_t>(count));
  feedLoopWDT();
  LOG_DBG("WEB", "Crash report chunk offset=%u bytes=%u total=%u", static_cast<unsigned>(offset),
          static_cast<unsigned>(count), static_cast<unsigned>(reportSize));
}

void CrossPointWebServer::handlePocketScreenPreview() const {
  HalSystem::setCrashBreadcrumb("nearby:screen-preview");
  if (!PocketDaily::Web::diagnosticsAffordable()) {
    server->send(503, "text/plain", "Reader memory is too low for diagnostics right now");
    return;
  }
  HalFile preview = Storage.open(PocketDaily::SCREEN_PREVIEW_PATH);
  if (!preview || preview.isDirectory()) {
    if (preview) preview.close();
    server->send(404, "text/plain", "No Pocket Daily screen preview available");
    return;
  }

  if (!server->hasArg("offset")) {
    preview.close();
    server->send(400, "text/plain", "Missing screen preview offset");
    return;
  }

  const String offsetText = server->arg("offset");
  if (offsetText.isEmpty()) {
    preview.close();
    server->send(400, "text/plain", "Invalid screen preview offset");
    return;
  }
  for (size_t i = 0; i < offsetText.length(); i++) {
    if (offsetText[i] < '0' || offsetText[i] > '9') {
      preview.close();
      server->send(400, "text/plain", "Invalid screen preview offset");
      return;
    }
  }

  const size_t previewSize = preview.size();
  const size_t offset = static_cast<size_t>(offsetText.toInt());
  if (offset >= previewSize || !preview.seek(offset)) {
    preview.close();
    server->send(416, "text/plain", "Screen preview offset out of range");
    return;
  }

  uint8_t* body = firmware_flash::sharedStagingBuffer();
  const size_t requested = std::min(previewSize - offset, SCREEN_PREVIEW_CHUNK_BYTES);
  const int count = preview.read(body, requested);
  preview.close();
  if (count <= 0) {
    server->send(500, "text/plain", "Could not read screen preview chunk");
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
  server->client().setTimeout(DIAGNOSTIC_SEND_TIMEOUT_MS);
  feedLoopWDT();
  server->setContentLength(static_cast<size_t>(count));
  server->sendHeader("Cache-Control", "no-store");
  server->send(200, "application/octet-stream", "");
  server->sendContent(reinterpret_cast<const char*>(body), static_cast<size_t>(count));
  feedLoopWDT();
  LOG_DBG("WEB", "Pocket screen preview offset=%u bytes=%u total=%u", static_cast<unsigned>(offset),
          static_cast<unsigned>(count), static_cast<unsigned>(previewSize));
}

void CrossPointWebServer::scanFiles(const char* path, const std::function<void(FileInfo)>& callback) const {
  HalFile root = Storage.open(path);
  if (!root) {
    LOG_DBG("WEB", "Failed to open directory: %s", path);
    return;
  }

  if (!root.isDirectory()) {
    LOG_DBG("WEB", "Not a directory: %s", path);
    root.close();
    return;
  }

  LOG_DBG("WEB", "Scanning files in: %s", path);

  HalFile file = root.openNextFile();
  char name[500];
  while (file) {
    file.getName(name, sizeof(name));
    auto fileName = String(name);

    // Skip hidden items (starting with ".")
    bool shouldHide = !SETTINGS.showHiddenFiles && fileName.startsWith(".");

    // Check against explicitly hidden items list
    if (!shouldHide) {
      for (const auto* item : HIDDEN_ITEMS) {
        if (fileName.equals(item)) {
          shouldHide = true;
          break;
        }
      }
    }

    if (!shouldHide) {
      FileInfo info;
      info.name = fileName;
      info.isDirectory = file.isDirectory();

      if (info.isDirectory) {
        info.size = 0;
        info.isEpub = false;
      } else {
        info.size = file.size();
        info.isEpub = isEpubFile(info.name);
      }

      callback(info);
    }

    file.close();
    yield();               // Yield to allow WiFi and other tasks to process during long scans
    esp_task_wdt_reset();  // Reset watchdog to prevent timeout on large directories
    file = root.openNextFile();
  }
  root.close();
}

bool CrossPointWebServer::isEpubFile(const String& filename) const { return FsHelpers::hasEpubExtension(filename); }

void CrossPointWebServer::handleFileList() const {
  sendHtmlContent(server.get(), FilesPageHtml, sizeof(FilesPageHtml));
}

void CrossPointWebServer::handleFileListData() const {
  // Get current path from query string (default to root)
  String currentPath = "/";
  if (server->hasArg("path")) {
    currentPath = server->arg("path");
    // Ensure path starts with /
    if (!currentPath.startsWith("/")) {
      currentPath = "/" + currentPath;
    }
    // Remove trailing slash unless it's root
    if (currentPath.length() > 1 && currentPath.endsWith("/")) {
      currentPath = currentPath.substring(0, currentPath.length() - 1);
    }
  }

  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("[");
  char output[512];
  constexpr size_t outputSize = sizeof(output);
  bool seenFirst = false;
  JsonDocument doc;

  scanFiles(currentPath.c_str(), [this, &output, &doc, seenFirst](const FileInfo& info) mutable {
    doc.clear();
    doc["name"] = info.name;
    doc["size"] = info.size;
    doc["isDirectory"] = info.isDirectory;
    doc["isEpub"] = info.isEpub;

    const size_t written = serializeJson(doc, output, outputSize);
    if (written >= outputSize) {
      // JSON output truncated; skip this entry to avoid sending malformed JSON
      LOG_DBG("WEB", "Skipping file entry with oversized JSON for name: %s", info.name.c_str());
      return;
    }

    if (seenFirst) {
      server->sendContent(",");
    } else {
      seenFirst = true;
    }
    server->sendContent(output);
  });
  server->sendContent("]");
  // End of streamed response, empty chunk to signal client
  server->sendContent("");
  LOG_DBG("WEB", "Served file listing page for path: %s", currentPath.c_str());
}

void CrossPointWebServer::handleDownload() const {
  if (!server->hasArg("path")) {
    server->send(400, "text/plain", "Missing path");
    return;
  }

  String itemPath = server->arg("path");
  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Invalid path");
    return;
  }
  if (!itemPath.startsWith("/")) {
    itemPath = "/" + itemPath;
  }

  const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);
  if (itemName.startsWith(".")) {
    server->send(403, "text/plain", "Cannot access system files");
    return;
  }
  for (const auto* item : HIDDEN_ITEMS) {
    if (itemName.equals(item)) {
      server->send(403, "text/plain", "Cannot access protected items");
      return;
    }
  }

  if (!Storage.exists(itemPath.c_str())) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  HalFile file = Storage.open(itemPath.c_str());
  if (!file) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  if (file.isDirectory()) {
    file.close();
    server->send(400, "text/plain", "Path is a directory");
    return;
  }

  String contentType = "application/octet-stream";
  if (isEpubFile(itemPath)) {
    contentType = "application/epub+zip";
  }

  char nameBuf[128] = {0};
  String filename = "download";
  if (file.getName(nameBuf, sizeof(nameBuf))) {
    filename = nameBuf;
  }

  server->setContentLength(file.size());
  server->sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  server->send(200, contentType.c_str(), "");

  NetworkClient client = server->client();
  // Keep the main-loop stack and lwIP heap from competing over a 4 KiB burst.
  // Downloads are throughput-insensitive compared with surviving the transfer.
  constexpr size_t chunkSize = 1024;
  uint8_t buffer[chunkSize];

  bool downloadOk = true;
  while (downloadOk && file.available()) {
    int result = file.read(buffer, chunkSize);
    if (result <= 0) break;
    size_t bytesRead = static_cast<size_t>(result);
    size_t totalWritten = 0;
    while (totalWritten < bytesRead) {
      esp_task_wdt_reset();
      size_t wrote = client.write(buffer + totalWritten, bytesRead - totalWritten);
      if (wrote == 0) {
        downloadOk = false;
        break;
      }
      totalWritten += wrote;
    }
  }
  client.clear();
  file.close();
}

// Diagnostic counters for upload performance analysis
static unsigned long uploadStartTime = 0;
static unsigned long totalWriteTime = 0;
static size_t writeCount = 0;

static bool flushUploadBuffer(CrossPointWebServer::UploadState& state) {
  if (state.bufferPos > 0 && state.file) {
    esp_task_wdt_reset();  // Reset watchdog before potentially slow SD write
    const unsigned long writeStart = millis();
    const size_t written = state.file.write(state.buffer.get(), state.bufferPos);
    totalWriteTime += millis() - writeStart;
    writeCount++;
    esp_task_wdt_reset();  // Reset watchdog after SD write

    if (written != state.bufferPos) {
      LOG_DBG("WEB", "[UPLOAD] Buffer flush failed: expected %d, wrote %d", state.bufferPos, written);
      state.bufferPos = 0;
      return false;
    }
    state.bufferPos = 0;
  }
  return true;
}

void CrossPointWebServer::handleUpload(UploadState& state) const {
  static size_t lastLoggedSize = 0;

  // Reset watchdog at start of every upload callback - HTTP parsing can be slow
  esp_task_wdt_reset();

  // Safety check: ensure server is still valid
  if (!running || !server) {
    LOG_DBG("WEB", "[UPLOAD] ERROR: handleUpload called but server not running!");
    return;
  }

  const HTTPUpload& upload = server->upload();

  if (upload.status == UPLOAD_FILE_START) {
    // Reset watchdog - this is the critical 1% crash point
    esp_task_wdt_reset();
    server->client().setTimeout(UPLOAD_SOCKET_TIMEOUT_MS);

    String requestedPath = server->hasArg("path") ? normalizeWebPath(server->arg("path")) : "/";
    const String requestedName = upload.filename;
    const bool chunked = server->hasArg("offset");
    size_t requestedOffset = 0;
    if (chunked && !parseUnsignedDecimal(server->arg("offset"), requestedOffset)) {
      state.success = false;
      state.error = "Invalid upload offset";
      pocketStream.publishHttpStaged(state.fileName, state.path, state.size, state.crc32, state.success, state.error,
                                     state.chunked, state.chunkStart);
      return;
    }

    if (chunked && requestedOffset > 0) {
      // A continuation is accepted only for the exact in-memory upload that
      // completed the preceding chunk. A process restart or mismatched client
      // starts again at offset zero instead of appending to unknown bytes.
      if (!state.chunked || !state.success || state.fileName != requestedName || state.path != requestedPath ||
          state.size != requestedOffset) {
        state.success = false;
        state.error = "Upload offset does not match staged data";
        pocketStream.publishHttpStaged(state.fileName, state.path, state.size, state.crc32, state.success, state.error,
                                       state.chunked, state.chunkStart);
        return;
      }
    } else {
      state.fileName = requestedName;
      state.path = requestedPath;
      state.size = 0;
      state.crc32 = 0xFFFFFFFFU;
    }

    state.chunked = chunked;
    state.chunkStart = requestedOffset;
    state.success = false;
    state.error = "";
    uploadStartTime = millis();
    lastLoggedSize = state.size;
    state.bufferPos = 0;
    totalWriteTime = 0;
    writeCount = 0;

    // Keep transfer buffers out of the idle server footprint. On X3 the Wi-Fi
    // driver dominates internal RAM; allocating only the active upload buffer
    // lets the Pocket profile and the legacy File Transfer profile coexist.
    // Chunked Pocket requests already arrive in framework-owned 1,436-byte
    // pieces and deliberately use direct writes, avoiding allocator churn
    // between every short request.
    state.buffer.reset();
    if (!chunked) state.buffer = makeUniqueNoThrow<uint8_t[]>(UploadState::UPLOAD_BUFFER_SIZE);
    if (!chunked && !state.buffer) {
      // The framework-owned upload buffer remains valid for this callback, so
      // a fragmented X3 can still stream each incoming piece straight to SD.
      // Batching is an optimization, never a requirement for accepting data.
      LOG_INF("WEB", "[UPLOAD] No %u-byte SD buffer; using direct writes", (unsigned)UploadState::UPLOAD_BUFFER_SIZE);
    }

    LOG_DBG("WEB", "[UPLOAD] START: %s path=%s offset=%u", state.fileName.c_str(), state.path.c_str(),
            static_cast<unsigned>(requestedOffset));
    LOG_DBG("WEB", "[UPLOAD] Free heap: %d bytes", ESP.getFreeHeap());

    // Create file path
    String filePath = state.path;
    if (!filePath.endsWith("/")) filePath += "/";
    filePath += state.fileName;

    esp_task_wdt_reset();
    if (chunked && requestedOffset > 0) {
      state.file = Storage.open(filePath.c_str(), O_RDWR);
      if (!state.file || state.file.size() != requestedOffset || !state.file.seekSet(requestedOffset)) {
        if (state.file) state.file.close();
        state.error = "Staged upload size changed";
        state.size = 0;
        state.crc32 = 0xFFFFFFFFU;
        Storage.remove(filePath.c_str());
        pocketStream.publishHttpStaged(state.fileName, state.path, state.size, state.crc32, state.success, state.error,
                                       state.chunked, state.chunkStart);
        return;
      }
    } else {
      // Offset zero and legacy uploads both intentionally replace stale staging
      // data. Final user-visible files are still protected by verified commit.
      if (Storage.exists(filePath.c_str())) Storage.remove(filePath.c_str());
      if (!Storage.openFileForWrite("WEB", filePath, state.file)) {
        state.error = "Failed to create file on SD card";
        state.buffer.reset();
        pocketStream.publishHttpStaged(state.fileName, state.path, state.size, state.crc32, state.success, state.error,
                                       state.chunked, state.chunkStart);
        LOG_DBG("WEB", "[UPLOAD] FAILED to create file: %s", filePath.c_str());
        return;
      }
    }
    esp_task_wdt_reset();

    pocketStream.publishHttpStaged(state.fileName, state.path, state.size, state.crc32, state.success, state.error,
                                   state.chunked, state.chunkStart);
    LOG_DBG("WEB", "[UPLOAD] File created successfully: %s", filePath.c_str());
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (state.file && state.error.isEmpty()) {
      state.crc32 = updateCrc32(state.crc32, upload.buf, upload.currentSize);
      const uint8_t* data = upload.buf;
      size_t remaining = upload.currentSize;

      if (state.buffer) {
        // Batch small incoming pieces when contiguous heap is available.
        while (remaining > 0) {
          const size_t space = UploadState::UPLOAD_BUFFER_SIZE - state.bufferPos;
          const size_t toCopy = (remaining < space) ? remaining : space;

          memcpy(state.buffer.get() + state.bufferPos, data, toCopy);
          state.bufferPos += toCopy;
          data += toCopy;
          remaining -= toCopy;

          if (state.bufferPos >= UploadState::UPLOAD_BUFFER_SIZE) {
            if (!flushUploadBuffer(state)) {
              state.error = "Failed to write to SD card - disk may be full";
              state.file.close();
              state.buffer.reset();
              return;
            }
          }
        }
      } else {
        // Allocation-free fallback. WebServer owns `upload.buf` until this
        // callback returns, so it is safe to write it synchronously.
        esp_task_wdt_reset();
        const unsigned long writeStart = millis();
        const size_t written = state.file.write(data, remaining);
        totalWriteTime += millis() - writeStart;
        writeCount++;
        esp_task_wdt_reset();
        if (written != remaining) {
          state.error = "Failed to write to SD card - disk may be full";
          state.file.close();
          return;
        }
      }

      state.size += upload.currentSize;

      // Log progress every 100KB
      if (state.size - lastLoggedSize >= 102400) {
        const unsigned long elapsed = millis() - uploadStartTime;
        const float kbps = (elapsed > 0) ? (state.size / 1024.0) / (elapsed / 1000.0) : 0;
        LOG_DBG("WEB", "[UPLOAD] %d bytes (%.1f KB), %.1f KB/s, %d writes", state.size, state.size / 1024.0, kbps,
                writeCount);
        lastLoggedSize = state.size;
      }
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (state.file) {
      // Flush any remaining buffered data
      if (!flushUploadBuffer(state)) {
        state.error = "Failed to write final data to SD card";
      }
      state.file.close();

      if (state.error.isEmpty()) {
        state.success = true;
        const unsigned long elapsed = millis() - uploadStartTime;
        const size_t requestBytes = state.size - state.chunkStart;
        const float avgKbps = (elapsed > 0) ? (requestBytes / 1024.0) / (elapsed / 1000.0) : 0;
        const float writePercent = (elapsed > 0) ? (totalWriteTime * 100.0 / elapsed) : 0;
        LOG_DBG("WEB", "[UPLOAD] Complete: %s chunk=%u total=%u in %lu ms, avg %.1f KB/s", state.fileName.c_str(),
                static_cast<unsigned>(requestBytes), static_cast<unsigned>(state.size), elapsed, avgKbps);
        LOG_DBG("WEB", "[UPLOAD] Diagnostics: %d writes, total write time: %lu ms (%.1f%%)", writeCount, totalWriteTime,
                writePercent);

        // Clear epub cache to prevent stale metadata issues when overwriting files
        String filePath = state.path;
        if (!filePath.endsWith("/")) filePath += "/";
        filePath += state.fileName;
        if (!state.chunked) clearBookCache(filePath.c_str());
      }
    }
    state.buffer.reset();
    pocketStream.publishHttpStaged(state.fileName, state.path, state.size, state.crc32, state.success, state.error,
                                   state.chunked, state.chunkStart);
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    state.bufferPos = 0;  // Discard buffered data
    if (state.file) {
      state.file.close();
      // Try to delete the incomplete file
      String filePath = state.path;
      if (!filePath.endsWith("/")) filePath += "/";
      filePath += state.fileName;
      Storage.remove(filePath.c_str());
    }
    if (state.chunked) {
      state.size = 0;
      state.crc32 = 0xFFFFFFFFU;
    }
    state.error = "Upload aborted";
    state.buffer.reset();
    pocketStream.publishHttpStaged(state.fileName, state.path, state.size, state.crc32, state.success, state.error,
                                   state.chunked, state.chunkStart);
    LOG_DBG("WEB", "Upload aborted");
  }
}

void CrossPointWebServer::handleUploadPost(UploadState& state) const {
  if (state.success) {
    server->send(200, "text/plain", "File uploaded successfully: " + state.fileName);
  } else {
    const String error = state.error.isEmpty() ? "Unknown error during upload" : state.error;
    server->send(400, "text/plain", error);
  }
}

void CrossPointWebServer::handleCommitUpload() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, server->arg("plain"));
  if (error || !doc["staging"].is<const char*>() || !doc["target"].is<const char*>() || !doc["size"].is<size_t>() ||
      !doc["crc32"].is<const char*>()) {
    server->send(400, "text/plain", "Invalid commit request");
    return;
  }

  const String staging = normalizeWebPath(doc["staging"].as<const char*>());
  const String target = normalizeWebPath(doc["target"].as<const char*>());
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
      targetName.isEmpty() || isProtectedItemName(targetName) || !crcEnd || *crcEnd != '\0' ||
      expectedCrcText.length() != 8) {
    server->send(400, "text/plain", "Unsafe commit path or checksum");
    return;
  }

  const PocketDaily::Web::StagedUpload& staged = pocketStream.staged();
  String uploadedPath = normalizeWebPath(staged.path + "/" + staged.fileName);
  const uint32_t uploadedCrc = staged.crc32 ^ 0xFFFFFFFFU;
  if (!staged.success || uploadedPath != staging || staged.size != expectedSize || uploadedCrc != expectedCrc ||
      !Storage.exists(staging.c_str())) {
    server->send(409, "text/plain", "Staged upload verification failed");
    return;
  }

  HalFile stagedFile = Storage.open(staging.c_str());
  const bool stagedSizeMatches = stagedFile && !stagedFile.isDirectory() && stagedFile.size() == expectedSize;
  if (stagedFile) stagedFile.close();
  if (!stagedSizeMatches) {
    server->send(409, "text/plain", "Staged file size mismatch");
    return;
  }

  const String backup = stagingParent + ".pocket-backup.part";
  Storage.remove(backup.c_str());
  const bool hadTarget = Storage.exists(target.c_str());
  if (hadTarget && !renameStorageFile(target, backup)) {
    server->send(500, "text/plain", "Could not preserve existing target");
    return;
  }
  if (!renameStorageFile(staging, target)) {
    if (hadTarget) renameStorageFile(backup, target);
    server->send(500, "text/plain", "Could not publish staged upload");
    return;
  }
  if (hadTarget) Storage.remove(backup.c_str());

  clearBookCache(target.c_str());
  char response[80];
  snprintf(response, sizeof(response), "{\"size\":%u,\"crc32\":\"%08lX\"}", static_cast<unsigned>(expectedSize),
           static_cast<unsigned long>(uploadedCrc));
  server->send(200, "application/json", response);
  LOG_INF("WEB", "Committed Pocket upload %s (%u bytes, crc32=%08lX)", target.c_str(),
          static_cast<unsigned>(expectedSize), static_cast<unsigned long>(uploadedCrc));
}

void CrossPointWebServer::handleCreateFolder() const {
  // Get folder name from form data
  if (!server->hasArg("name")) {
    server->send(400, "text/plain", "Missing folder name");
    return;
  }

  const String folderName = server->arg("name");

  // Validate folder name
  if (folderName.isEmpty()) {
    server->send(400, "text/plain", "Folder name cannot be empty");
    return;
  }

  // Get parent path
  String parentPath = "/";
  if (server->hasArg("path")) {
    parentPath = server->arg("path");
    if (!parentPath.startsWith("/")) {
      parentPath = "/" + parentPath;
    }
    if (parentPath.length() > 1 && parentPath.endsWith("/")) {
      parentPath = parentPath.substring(0, parentPath.length() - 1);
    }
  }

  // Build full folder path
  String folderPath = parentPath;
  if (!folderPath.endsWith("/")) folderPath += "/";
  folderPath += folderName;

  LOG_DBG("WEB", "Creating folder: %s", folderPath.c_str());

  // Check if already exists
  if (Storage.exists(folderPath.c_str())) {
    server->send(400, "text/plain", "Folder already exists");
    return;
  }

  // Create the folder
  if (Storage.mkdir(folderPath.c_str())) {
    LOG_DBG("WEB", "Folder created successfully: %s", folderPath.c_str());
    server->send(200, "text/plain", "Folder created: " + folderName);
  } else {
    LOG_DBG("WEB", "Failed to create folder: %s", folderPath.c_str());
    server->send(500, "text/plain", "Failed to create folder");
  }
}

void CrossPointWebServer::handleRename() const {
  if (!server->hasArg("path") || !server->hasArg("name")) {
    server->send(400, "text/plain", "Missing path or new name");
    return;
  }

  String itemPath = normalizeWebPath(server->arg("path"));
  String newName = server->arg("name");
  newName.trim();

  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Invalid path");
    return;
  }
  if (newName.isEmpty()) {
    server->send(400, "text/plain", "New name cannot be empty");
    return;
  }
  if (newName.indexOf('/') >= 0 || newName.indexOf('\\') >= 0) {
    server->send(400, "text/plain", "Invalid file name");
    return;
  }
  if (isProtectedItemName(newName)) {
    server->send(403, "text/plain", "Cannot rename to protected name");
    return;
  }

  const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);
  if (isProtectedItemName(itemName)) {
    server->send(403, "text/plain", "Cannot rename protected item");
    return;
  }
  if (newName == itemName) {
    server->send(200, "text/plain", "Name unchanged");
    return;
  }

  if (!Storage.exists(itemPath.c_str())) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  HalFile file = Storage.open(itemPath.c_str());
  if (!file) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  if (file.isDirectory()) {
    file.close();
    server->send(400, "text/plain", "Only files can be renamed");
    return;
  }

  String parentPath = itemPath.substring(0, itemPath.lastIndexOf('/'));
  if (parentPath.isEmpty()) {
    parentPath = "/";
  }
  String newPath = parentPath;
  if (!newPath.endsWith("/")) {
    newPath += "/";
  }
  newPath += newName;

  if (Storage.exists(newPath.c_str())) {
    file.close();
    server->send(409, "text/plain", "Target already exists");
    return;
  }

  clearBookCache(itemPath.c_str());
  const bool success = file.rename(newPath.c_str());
  file.close();

  if (success) {
    LOG_DBG("WEB", "Renamed file: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(200, "text/plain", "Renamed successfully");
  } else {
    LOG_ERR("WEB", "Failed to rename file: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(500, "text/plain", "Failed to rename file");
  }
}

void CrossPointWebServer::handleMove() const {
  if (!server->hasArg("path") || !server->hasArg("dest")) {
    server->send(400, "text/plain", "Missing path or destination");
    return;
  }

  String itemPath = normalizeWebPath(server->arg("path"));
  String destPath = normalizeWebPath(server->arg("dest"));

  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Invalid path");
    return;
  }
  if (destPath.isEmpty()) {
    server->send(400, "text/plain", "Invalid destination");
    return;
  }

  const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);
  if (isProtectedItemName(itemName)) {
    server->send(403, "text/plain", "Cannot move protected item");
    return;
  }
  if (destPath != "/") {
    const String destName = destPath.substring(destPath.lastIndexOf('/') + 1);
    if (isProtectedItemName(destName)) {
      server->send(403, "text/plain", "Cannot move into protected folder");
      return;
    }
  }

  if (!Storage.exists(itemPath.c_str())) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  HalFile file = Storage.open(itemPath.c_str());
  if (!file) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  if (file.isDirectory()) {
    file.close();
    server->send(400, "text/plain", "Only files can be moved");
    return;
  }

  if (!Storage.exists(destPath.c_str())) {
    file.close();
    server->send(404, "text/plain", "Destination not found");
    return;
  }
  HalFile destDir = Storage.open(destPath.c_str());
  if (!destDir || !destDir.isDirectory()) {
    if (destDir) {
      destDir.close();
    }
    file.close();
    server->send(400, "text/plain", "Destination is not a folder");
    return;
  }
  destDir.close();

  String newPath = destPath;
  if (!newPath.endsWith("/")) {
    newPath += "/";
  }
  newPath += itemName;

  if (newPath == itemPath) {
    file.close();
    server->send(200, "text/plain", "Already in destination");
    return;
  }
  if (Storage.exists(newPath.c_str())) {
    file.close();
    server->send(409, "text/plain", "Target already exists");
    return;
  }

  clearBookCache(itemPath.c_str());
  const bool success = file.rename(newPath.c_str());
  file.close();

  if (success) {
    LOG_DBG("WEB", "Moved file: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(200, "text/plain", "Moved successfully");
  } else {
    LOG_ERR("WEB", "Failed to move file: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(500, "text/plain", "Failed to move file");
  }
}

void CrossPointWebServer::handleDelete() const {
  // To ensure backwards compatibility, plain `path` is mapped
  // to a single element JSON array.
  bool hasPathArg = server->hasArg("path");
  bool hasPathsArg = server->hasArg("paths");
  // Check 'paths' or `path` argument is provided
  if (!(hasPathArg || hasPathsArg)) {
    server->send(400, "text/plain", "Missing `path` or `paths` argument");
    return;
  }
  if (hasPathArg && hasPathsArg) {
    server->send(400, "text/plain", "Provide either 'path' or 'paths', not both");
    return;
  }

  // Parse paths
  String pathsArg;
  JsonDocument doc;
  DeserializationError error = DeserializationError(DeserializationError::Code::Ok);
  if (hasPathsArg) {
    pathsArg = server->arg("paths");
    error = deserializeJson(doc, pathsArg);
  } else {
    pathsArg = server->arg("path");
    doc.add(pathsArg);
  }
  if (error) {
    server->send(400, "text/plain", "Invalid paths format");
    return;
  }

  auto paths = doc.as<JsonArray>();
  if (paths.isNull() || paths.size() == 0) {
    server->send(400, "text/plain", "No paths provided");
    return;
  }

  // Iterate over paths and delete each item
  bool allSuccess = true;
  String failedItems;

  for (const auto& p : paths) {
    auto itemPath = p.as<String>();

    // Validate path
    if (itemPath.isEmpty() || itemPath == "/") {
      failedItems += itemPath + " (cannot delete root); ";
      allSuccess = false;
      continue;
    }

    // Ensure path starts with /
    if (!itemPath.startsWith("/")) {
      itemPath = "/" + itemPath;
    }

    // Security check: prevent deletion of protected items
    const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);

    // Hidden/system files are protected
    if (itemName.startsWith(".")) {
      failedItems += itemPath + " (hidden/system file); ";
      allSuccess = false;
      continue;
    }

    // Check against explicitly protected items
    bool isProtected = false;
    for (const auto* item : HIDDEN_ITEMS) {
      if (itemName.equals(item)) {
        isProtected = true;
        break;
      }
    }
    if (isProtected) {
      failedItems += itemPath + " (protected file); ";
      allSuccess = false;
      continue;
    }

    // Check if item exists
    if (!Storage.exists(itemPath.c_str())) {
      failedItems += itemPath + " (not found); ";
      allSuccess = false;
      continue;
    }

    // Decide whether it's a directory or file by opening it
    bool success = false;
    HalFile f = Storage.open(itemPath.c_str());
    if (f && f.isDirectory()) {
      // For folders, ensure empty before removing
      HalFile entry = f.openNextFile();
      if (entry) {
        entry.close();
        f.close();
        failedItems += itemPath + " (folder not empty); ";
        allSuccess = false;
        continue;
      }
      f.close();
      success = Storage.rmdir(itemPath.c_str());
    } else {
      // It's a file (or couldn't open as dir) — remove file
      if (f) f.close();
      success = Storage.remove(itemPath.c_str());
      clearBookCache(itemPath.c_str());
    }

    if (!success) {
      failedItems += itemPath + " (deletion failed); ";
      allSuccess = false;
    }
  }

  if (allSuccess) {
    server->send(200, "text/plain", "All items deleted successfully");
  } else {
    server->send(500, "text/plain", "Failed to delete some items: " + failedItems);
  }
}

void CrossPointWebServer::handleSettingsPage() const {
  sendHtmlContent(server.get(), SettingsPageHtml, sizeof(SettingsPageHtml));
  LOG_DBG("WEB", "Served settings page");
}

void CrossPointWebServer::handleGetSettings() const {
  // Stream the static base settings list directly. Building the registry-aware
  // copy allocates a large vector, which can collide with File Transfer upload
  // buffers on no-PSRAM ESP32-C3 boards.
  const auto& settings = getBaseSettingsList();
  const auto& fontRegistry = sdFontSystem.registry();

  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("[");

  char output[512];
  constexpr size_t outputSize = sizeof(output);
  bool seenFirst = false;
  JsonDocument doc;

  for (const auto& s : settings) {
    if (!s.key) continue;  // Skip ACTION-only entries

    doc.clear();
    doc["key"] = s.key;
    doc["name"] = I18N.get(s.nameId);
    doc["category"] = I18N.get(s.category);

    switch (s.type) {
      case SettingType::TOGGLE: {
        doc["type"] = "toggle";
        if (s.valuePtr) {
          doc["value"] = static_cast<int>(SETTINGS.*(s.valuePtr));
        }
        break;
      }
      case SettingType::ENUM: {
        doc["type"] = "enum";
        const bool isFontFamily = s.key && strcmp(s.key, "fontFamily") == 0;
        if (isFontFamily && fontRegistry.getFamilyCount() > 0) {
          uint8_t value = SETTINGS.fontFamily < CrossPointSettings::BUILTIN_FONT_COUNT ? SETTINGS.fontFamily : 0;
          if (SETTINGS.sdFontFamilyName[0] != '\0') {
            const auto& families = fontRegistry.getFamilies();
            for (int i = 0; i < static_cast<int>(families.size()); i++) {
              if (families[i].name == SETTINGS.sdFontFamilyName) {
                value = static_cast<uint8_t>(CrossPointSettings::BUILTIN_FONT_COUNT + i);
                break;
              }
            }
          }
          doc["value"] = static_cast<int>(value);
        } else if (s.valuePtr) {
          doc["value"] = static_cast<int>(SETTINGS.*(s.valuePtr));
        } else if (s.valueGetter) {
          doc["value"] = static_cast<int>(s.valueGetter());
        }
        JsonArray options = doc["options"].to<JsonArray>();
        if (isFontFamily && fontRegistry.getFamilyCount() > 0) {
          options.add(I18N.get(StrId::STR_NOTO_SERIF));
          options.add(I18N.get(StrId::STR_NOTO_SANS));
          for (const auto& family : fontRegistry.getFamilies()) {
            options.add(family.name);
          }
        } else if (!s.enumStringValues.empty()) {
          for (const auto& opt : s.enumStringValues) {
            options.add(opt);
          }
        } else {
          for (const auto& opt : s.enumValues) {
            options.add(I18N.get(opt));
          }
        }
        break;
      }
      case SettingType::VALUE: {
        doc["type"] = "value";
        if (s.valuePtr) {
          doc["value"] = static_cast<int>(SETTINGS.*(s.valuePtr));
        }
        doc["min"] = s.valueRange.min;
        doc["max"] = s.valueRange.max;
        doc["step"] = s.valueRange.step;
        break;
      }
      case SettingType::STRING: {
        doc["type"] = "string";
        if (s.stringGetter) {
          doc["value"] = s.stringGetter();
        } else if (s.stringMaxLen > 0) {
          doc["value"] = reinterpret_cast<const char*>(&SETTINGS) + s.stringOffset;
        }
        break;
      }
      default:
        continue;
    }

    const size_t written = serializeJson(doc, output, outputSize);
    if (written >= outputSize) {
      LOG_DBG("WEB", "Skipping oversized setting JSON for: %s", s.key);
      continue;
    }

    if (seenFirst) {
      server->sendContent(",");
    } else {
      seenFirst = true;
    }
    server->sendContent(output);
  }

  server->sendContent("]");
  server->sendContent("");
  LOG_DBG("WEB", "Served settings API");
}

void CrossPointWebServer::handlePostSettings() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  const auto& settings = getSettingsList(&sdFontSystem.registry());
  int applied = 0;

  for (const auto& s : settings) {
    if (!s.key) continue;
    if (!doc[s.key].is<JsonVariant>()) continue;

    switch (s.type) {
      case SettingType::TOGGLE: {
        const int val = doc[s.key].as<int>() ? 1 : 0;
        if (s.valuePtr) {
          SETTINGS.*(s.valuePtr) = val;
        }
        applied++;
        break;
      }
      case SettingType::ENUM: {
        const int val = doc[s.key].as<int>();
        const int maxVal = s.enumStringValues.empty() ? static_cast<int>(s.enumValues.size())
                                                      : static_cast<int>(s.enumStringValues.size());
        if (val >= 0 && val < maxVal) {
          if (s.valuePtr) {
            SETTINGS.*(s.valuePtr) = static_cast<uint8_t>(val);
          } else if (s.valueSetter) {
            s.valueSetter(static_cast<uint8_t>(val));
          }
          applied++;
        }
        break;
      }
      case SettingType::VALUE: {
        const int val = doc[s.key].as<int>();
        if (val >= s.valueRange.min && val <= s.valueRange.max) {
          if (s.valuePtr) {
            SETTINGS.*(s.valuePtr) = static_cast<uint8_t>(val);
          }
          applied++;
        }
        break;
      }
      case SettingType::STRING: {
        const std::string val = doc[s.key].as<std::string>();
        if (s.stringSetter) {
          s.stringSetter(val);
        } else if (s.stringMaxLen > 0) {
          char* ptr = reinterpret_cast<char*>(&SETTINGS) + s.stringOffset;
          strncpy(ptr, val.c_str(), s.stringMaxLen - 1);
          ptr[s.stringMaxLen - 1] = '\0';
        }
        applied++;
        break;
      }
      default:
        break;
    }
  }

  SETTINGS.saveToFile();

  LOG_DBG("WEB", "Applied %d setting(s)", applied);
  server->send(200, "text/plain", String("Applied ") + String(applied) + " setting(s)");
}

void CrossPointWebServer::handleGetPocketPreferences() const {
  char json[192];
  const int written = snprintf(
      json, sizeof(json), "{\"startupApp\":%u,\"pocketDailySleepCover\":%u,\"sleepTimeoutMinutes\":%u,\"fontSize\":%u}",
      static_cast<unsigned>(SETTINGS.startupApp), static_cast<unsigned>(SETTINGS.pocketDailySleepCover),
      static_cast<unsigned>(SETTINGS.sleepTimeoutMinutes), static_cast<unsigned>(SETTINGS.fontSize));
  if (written <= 0 || static_cast<size_t>(written) >= sizeof(json)) {
    server->send(500, "text/plain", "Could not encode Pocket preferences");
    return;
  }
  server->sendHeader("Connection", "close");
  server->send(200, "application/json", json);
}

void CrossPointWebServer::handlePostPocketPreferences() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, server->arg("plain"));
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  if (!doc["startupApp"].isNull()) {
    const int value = doc["startupApp"].as<int>();
    if (value < 0 || value >= CrossPointSettings::STARTUP_APP_COUNT) {
      server->send(400, "text/plain", "Invalid startupApp");
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
      server->send(400, "text/plain", "Invalid sleepTimeoutMinutes");
      return;
    }
    SETTINGS.sleepTimeoutMinutes = static_cast<uint8_t>(value);
  }
  if (!doc["fontSize"].isNull()) {
    const int value = doc["fontSize"].as<int>();
    if (value < 0 || value >= CrossPointSettings::FONT_SIZE_COUNT) {
      server->send(400, "text/plain", "Invalid fontSize");
      return;
    }
    SETTINGS.fontSize = static_cast<uint8_t>(value);
  }

  if (!SETTINGS.saveToFile()) {
    server->send(500, "text/plain", "Could not save Pocket preferences");
    return;
  }
  server->sendHeader("Connection", "close");
  server->send(200, "application/json", "{\"saved\":true}");
  liveStudio.notifyPrefsChanged();
}

// ---- OPDS Server API ----

void CrossPointWebServer::handleGetOpdsServers() const {
  const auto& servers = OPDS_STORE.getServers();

  // Stream JSON array incrementally to avoid allocating the full response in memory
  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("[");

  char output[512];
  constexpr size_t outputSize = sizeof(output);
  JsonDocument doc;

  for (size_t i = 0; i < servers.size(); i++) {
    doc.clear();
    doc["index"] = i;
    doc["name"] = servers[i].name;
    doc["url"] = servers[i].url;
    doc["username"] = servers[i].username;
    // Never expose passwords over the API — only indicate whether one is set
    doc["hasPassword"] = !servers[i].password.empty();

    const size_t written = serializeJson(doc, output, outputSize);
    if (written >= outputSize) continue;

    if (i > 0) server->sendContent(",");
    server->sendContent(output);
  }

  server->sendContent("]");
  server->sendContent("");
  LOG_DBG("WEB", "Served OPDS servers API (%zu servers)", servers.size());
}

void CrossPointWebServer::handlePostOpdsServer() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  OpdsServer opdsServer;
  opdsServer.name = doc["name"] | std::string("");
  opdsServer.url = doc["url"] | std::string("");
  opdsServer.username = doc["username"] | std::string("");

  // The password field is optional in the JSON payload. When absent (vs. present but empty),
  // we preserve the existing password — the web UI omits it when the user hasn't changed it.
  bool hasPasswordField = doc["password"].is<const char*>() || doc["password"].is<std::string>();
  std::string password = doc["password"] | std::string("");

  if (doc["index"].is<int>()) {
    int idx = doc["index"].as<int>();
    if (idx < 0 || idx >= static_cast<int>(OPDS_STORE.getCount())) {
      server->send(400, "text/plain", "Invalid server index");
      return;
    }
    // Preserve existing password if not explicitly provided
    if (!hasPasswordField) {
      const auto* existing = OPDS_STORE.getServer(static_cast<size_t>(idx));
      if (existing) password = existing->password;
    }
    opdsServer.password = password;
    OPDS_STORE.updateServer(static_cast<size_t>(idx), opdsServer);
    LOG_DBG("WEB", "Updated OPDS server at index %d", idx);
  } else {
    opdsServer.password = password;
    if (!OPDS_STORE.addServer(opdsServer)) {
      server->send(400, "text/plain", "Cannot add server (limit reached)");
      return;
    }
    LOG_DBG("WEB", "Added new OPDS server: %s", opdsServer.name.c_str());
  }

  server->send(200, "text/plain", "OK");
}

// Uses POST (not HTTP DELETE) because ESP32 WebServer doesn't support DELETE with body.
void CrossPointWebServer::handleDeleteOpdsServer() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  if (!doc["index"].is<int>()) {
    server->send(400, "text/plain", "Missing index");
    return;
  }

  int idx = doc["index"].as<int>();
  if (idx < 0 || idx >= static_cast<int>(OPDS_STORE.getCount())) {
    server->send(400, "text/plain", "Invalid server index");
    return;
  }

  OPDS_STORE.removeServer(static_cast<size_t>(idx));
  LOG_DBG("WEB", "Deleted OPDS server at index %d", idx);
  server->send(200, "text/plain", "OK");
}

// ---- Wi-Fi Credentials API ----

void CrossPointWebServer::handleGetWifiNetworks() const {
  const auto& credentials = WIFI_STORE.getCredentials();
  const std::string& lastConnectedSsid = WIFI_STORE.getLastConnectedSsid();

  // Stream JSON array incrementally to avoid allocating the full response in memory
  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("[");

  char output[320];
  constexpr size_t outputSize = sizeof(output);
  JsonDocument doc;

  for (size_t i = 0; i < credentials.size(); i++) {
    doc.clear();
    doc["index"] = i;
    doc["ssid"] = credentials[i].ssid;
    // Never expose Wi-Fi passwords over the API — only indicate whether one is set
    doc["hasPassword"] = !credentials[i].password.empty();
    doc["isLastConnected"] = credentials[i].ssid == lastConnectedSsid;

    const size_t written = serializeJson(doc, output, outputSize);
    if (written >= outputSize) continue;

    if (i > 0) server->sendContent(",");
    server->sendContent(output);
  }

  server->sendContent("]");
  server->sendContent("");
  LOG_DBG("WEB", "Served Wi-Fi credentials API (%zu network(s))", credentials.size());
}

void CrossPointWebServer::handlePostWifiNetwork() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  std::string ssid = doc["ssid"] | std::string("");
  if (ssid.empty()) {
    server->send(400, "text/plain", "SSID is required");
    return;
  }

  // The password field is optional in the JSON payload. When absent (vs. present but empty),
  // preserve the existing password for updates. Empty passwords are valid for open networks.
  bool hasPasswordField = doc["password"].is<const char*>() || doc["password"].is<std::string>();
  std::string password = doc["password"] | std::string("");

  if (doc["index"].is<int>()) {
    int idx = doc["index"].as<int>();
    const auto& credentials = WIFI_STORE.getCredentials();
    if (idx < 0 || idx >= static_cast<int>(credentials.size())) {
      server->send(400, "text/plain", "Invalid network index");
      return;
    }

    const std::string oldSsid = credentials[static_cast<size_t>(idx)].ssid;
    if (!hasPasswordField) {
      password = credentials[static_cast<size_t>(idx)].password;
    }

    bool ok = true;
    if (oldSsid != ssid) {
      ok = WIFI_STORE.removeCredential(oldSsid) && WIFI_STORE.addCredential(ssid, password);
    } else {
      ok = WIFI_STORE.addCredential(ssid, password);
    }

    if (!ok) {
      server->send(400, "text/plain", "Failed to update Wi-Fi network");
      return;
    }

    LOG_DBG("WEB", "Updated Wi-Fi network at index %d (SSID: %s)", idx, ssid.c_str());
  } else {
    if (!WIFI_STORE.addCredential(ssid, password)) {
      server->send(400, "text/plain", "Cannot add network (limit reached)");
      return;
    }
    LOG_DBG("WEB", "Added Wi-Fi network: %s", ssid.c_str());
  }

  server->send(200, "text/plain", "OK");
}

// Uses POST (not HTTP DELETE) because ESP32 WebServer doesn't support DELETE with body.
void CrossPointWebServer::handleDeleteWifiNetwork() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  if (!doc["index"].is<int>()) {
    server->send(400, "text/plain", "Missing index");
    return;
  }

  int idx = doc["index"].as<int>();
  const auto& credentials = WIFI_STORE.getCredentials();
  if (idx < 0 || idx >= static_cast<int>(credentials.size())) {
    server->send(400, "text/plain", "Invalid network index");
    return;
  }

  const std::string ssid = credentials[static_cast<size_t>(idx)].ssid;
  if (!WIFI_STORE.removeCredential(ssid)) {
    server->send(400, "text/plain", "Failed to delete Wi-Fi network");
    return;
  }

  LOG_DBG("WEB", "Deleted Wi-Fi network at index %d (SSID: %s)", idx, ssid.c_str());
  server->send(200, "text/plain", "OK");
}

// WebSocket callback trampoline
void CrossPointWebServer::wsEventCallback(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  if (wsInstance) {
    wsInstance->onWebSocketEvent(num, type, payload, length);
  }
}

// WebSocket event handler for fast binary uploads
// Protocol:
//   1. Client sends TEXT message: "START:<filename>:<size>:<path>"
//   2. Client sends BINARY messages with file data chunks
//   3. Server sends TEXT "PROGRESS:<received>:<total>" after each chunk
//   4. Server sends TEXT "DONE" or "ERROR:<message>" when complete
void CrossPointWebServer::onWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      LOG_DBG("WS", "Client %u disconnected", num);
      liveStudio.onWsDisconnected(num);
      // Only clean up if this is the client that owns the active upload.
      // A new client may have already started a fresh upload before this
      // DISCONNECTED event fires (race condition on quick cancel + retry).
      if (num == wsUploadClientNum && wsUploadInProgress && wsUploadFile) {
        abortWsUpload("WS");
      }
      break;
    case WStype_CONNECTED: {
      liveStudio.onWsConnected(num);
      break;
    }

    case WStype_TEXT: {
      // Live Studio v1 JSON control messages win over the legacy upload
      // grammar on every profile; the service reports whether it consumed
      // the payload.
      if (liveStudio.onWsText(num, payload, length)) break;
      if (profile != CrossPointWebServerProfile::FULL) break;

      // Parse control messages
      String msg = String((char*)payload);
      LOG_DBG("WS", "Text from client %u: %s", num, msg.c_str());

      if (msg.startsWith("START:")) {
        // Reject any START while an upload is already active to prevent
        // leaking the open wsUploadFile handle (owning client re-START included)
        if (wsUploadInProgress) {
          wsServer->sendTXT(num, "ERROR:Upload already in progress");
          break;
        }

        // Parse: START:<filename>:<size>:<path>
        int firstColon = msg.indexOf(':', 6);
        int secondColon = msg.indexOf(':', firstColon + 1);

        if (firstColon > 0 && secondColon > 0) {
          wsUploadFileName = msg.substring(6, firstColon);
          String sizeToken = msg.substring(firstColon + 1, secondColon);
          bool sizeValid = sizeToken.length() > 0;
          int digitStart = (sizeValid && sizeToken[0] == '+') ? 1 : 0;
          if (digitStart > 0 && sizeToken.length() < 2) sizeValid = false;
          for (int i = digitStart; i < (int)sizeToken.length() && sizeValid; i++) {
            if (!isdigit((unsigned char)sizeToken[i])) sizeValid = false;
          }
          if (!sizeValid) {
            LOG_DBG("WS", "START rejected: invalid size token '%s'", sizeToken.c_str());
            wsServer->sendTXT(num, "ERROR:Invalid START format");
            return;
          }
          wsUploadSize = sizeToken.toInt();
          wsUploadPath = msg.substring(secondColon + 1);
          wsUploadReceived = 0;
          wsLastProgressSent = 0;
          wsUploadStartTime = millis();

          // Ensure path is valid
          if (!wsUploadPath.startsWith("/")) wsUploadPath = "/" + wsUploadPath;
          if (wsUploadPath.length() > 1 && wsUploadPath.endsWith("/")) {
            wsUploadPath = wsUploadPath.substring(0, wsUploadPath.length() - 1);
          }

          // Build file path
          String filePath = wsUploadPath;
          if (!filePath.endsWith("/")) filePath += "/";
          filePath += wsUploadFileName;

          LOG_DBG("WS", "Starting upload: %s (%d bytes) to %s", wsUploadFileName.c_str(), wsUploadSize,
                  filePath.c_str());

          // Check if file exists and remove it
          esp_task_wdt_reset();
          if (Storage.exists(filePath.c_str())) {
            Storage.remove(filePath.c_str());
          }

          // Open file for writing
          esp_task_wdt_reset();
          if (!Storage.openFileForWrite("WS", filePath, wsUploadFile)) {
            wsServer->sendTXT(num, "ERROR:Failed to create file");
            wsUploadInProgress = false;
            wsUploadClientNum = 255;
            return;
          }
          esp_task_wdt_reset();

          // Zero-byte upload: complete immediately without waiting for BIN frames
          if (wsUploadSize == 0) {
            // Explicit close() required: file-scope global persists beyond function scope
            wsUploadFile.close();
            wsLastCompleteName = wsUploadFileName;
            wsLastCompleteSize = 0;
            wsLastCompleteAt = millis();
            LOG_DBG("WS", "Zero-byte upload complete: %s", filePath.c_str());
            clearBookCache(filePath.c_str());
            wsServer->sendTXT(num, "DONE");
            wsLastProgressSent = 0;
            break;
          }

          wsUploadClientNum = num;
          wsUploadInProgress = true;
          wsServer->sendTXT(num, "READY");
        } else {
          wsServer->sendTXT(num, "ERROR:Invalid START format");
        }
      }
      break;
    }

    case WStype_BIN: {
      // The binary upload path belongs to the legacy browser surface only.
      if (profile != CrossPointWebServerProfile::FULL) break;
      if (!wsUploadInProgress || !wsUploadFile || num != wsUploadClientNum) {
        wsServer->sendTXT(num, "ERROR:No upload in progress");
        return;
      }

      // Write binary data directly to file
      size_t remaining = wsUploadSize - wsUploadReceived;
      if (length > remaining) {
        abortWsUpload("WS");
        wsServer->sendTXT(num, "ERROR:Upload overflow");
        return;
      }
      esp_task_wdt_reset();
      size_t written = wsUploadFile.write(payload, length);
      esp_task_wdt_reset();

      if (written != length) {
        abortWsUpload("WS");
        wsServer->sendTXT(num, "ERROR:Write failed - disk full?");
        return;
      }

      wsUploadReceived += written;

      // Send progress update (every 64KB or at end)
      if (wsUploadReceived - wsLastProgressSent >= 65536 || wsUploadReceived >= wsUploadSize) {
        String progress = "PROGRESS:" + String(wsUploadReceived) + ":" + String(wsUploadSize);
        wsServer->sendTXT(num, progress);
        wsLastProgressSent = wsUploadReceived;
      }

      // Check if upload complete
      if (wsUploadReceived >= wsUploadSize) {
        // Explicit close() required: file-scope global persists beyond function scope
        wsUploadFile.close();
        wsUploadInProgress = false;
        wsUploadClientNum = 255;

        wsLastCompleteName = wsUploadFileName;
        wsLastCompleteSize = wsUploadSize;
        wsLastCompleteAt = millis();

        unsigned long elapsed = millis() - wsUploadStartTime;
        float kbps = (elapsed > 0) ? (wsUploadSize / 1024.0) / (elapsed / 1000.0) : 0;

        LOG_DBG("WS", "Upload complete: %s (%d bytes in %lu ms, %.1f KB/s)", wsUploadFileName.c_str(), wsUploadSize,
                elapsed, kbps);

        // Clear epub cache to prevent stale metadata issues when overwriting files
        String filePath = wsUploadPath;
        if (!filePath.endsWith("/")) filePath += "/";
        filePath += wsUploadFileName;
        clearBookCache(filePath.c_str());

        wsServer->sendTXT(num, "DONE");
        wsLastProgressSent = 0;
      }
      break;
    }

    default:
      break;
  }
}

// --- Font management handlers ---

void CrossPointWebServer::handleFontsPage() const {
  sendHtmlContent(server.get(), FontsPageHtml, sizeof(FontsPageHtml));
  LOG_DBG("WEB", "Served fonts page");
}

void CrossPointWebServer::handleFontList() const {
  // Pick up any uploads/deletes that happened since the last reader load.
  const_cast<SdCardFontSystem&>(sdFontSystem).refreshIfDirty();
  const auto& families = sdFontSystem.registry().getFamilies();

  JsonDocument doc;
  JsonArray arr = doc["families"].to<JsonArray>();
  doc["maxFamilies"] = SdCardFontRegistry::MAX_SD_FAMILIES;

  for (const auto& family : families) {
    JsonObject fObj = arr.add<JsonObject>();
    fObj["name"] = family.name;

    JsonArray sizes = fObj["sizes"].to<JsonArray>();
    for (uint8_t s : family.availableSizes()) {
      sizes.add(s);
    }

    JsonArray files = fObj["files"].to<JsonArray>();
    for (const auto& file : family.files) {
      JsonObject fileObj = files.add<JsonObject>();
      // Extract filename from full path
      const char* name = strrchr(file.path.c_str(), '/');
      fileObj["name"] = name ? name + 1 : file.path.c_str();

      // Stat the file for size
      HalFile f;
      if (Storage.openFileForRead("WEB", file.path.c_str(), f)) {
        fileObj["size"] = static_cast<unsigned long>(f.size());
        f.close();
      } else {
        fileObj["size"] = 0;
      }
    }
  }

  String json;
  serializeJson(doc, json);
  server->send(200, "application/json", json);
}

void CrossPointWebServer::handleFontUploadData() {
  HTTPUpload& upload = server->upload();

  switch (upload.status) {
    case UPLOAD_FILE_START: {
      esp_task_wdt_reset();
      String family = server->arg("family");
      fontUpload.file = HalFile();
      fontUpload.familyName.clear();
      fontUpload.filePath.clear();
      fontUpload.valid = false;
      fontUpload.magicChecked = false;
      fontUpload.bytesWritten = 0;
      fontUpload.bufferPos = 0;
      fontUpload.buffer.reset();

      if (!FontInstaller::isValidFamilyName(family.c_str())) {
        LOG_ERR("WEB", "Invalid font family name: %s", family.c_str());
        break;
      }

      String filename = upload.filename;
      filename.replace(' ', '_');
      // Validate filename: rejects path traversal (../, /, \) and enforces
      // a .cpfont basename of alphanumeric + hyphen + underscore. Without
      // this an attacker could supply "../../.crosspoint/settings.json" as
      // a "filename" and have it written outside the fonts directory.
      if (!FontInstaller::isValidCpfontFilename(filename.c_str())) {
        LOG_ERR("WEB", "Invalid font filename: %s", filename.c_str());
        break;
      }

      fontUpload.familyName = family.c_str();

      // Create a temporary FontInstaller for directory creation
      FontInstaller installer(sdFontSystem.registry());
      if (!installer.ensureFamilyDir(family.c_str())) {
        LOG_ERR("WEB", "Failed to create font family dir");
        break;
      }

      char path[128];
      FontInstaller::buildFontPath(family.c_str(), filename.c_str(), path, sizeof(path));
      fontUpload.filePath = path;

      fontUpload.buffer = makeUniqueNoThrow<uint8_t[]>(FontUploadState::BUFFER_SIZE);
      if (!fontUpload.buffer) {
        LOG_ERR("WEB", "Not enough memory for font upload buffer");
        break;
      }

      if (!Storage.openFileForWrite("WEB", path, fontUpload.file)) {
        fontUpload.buffer.reset();
        LOG_ERR("WEB", "Failed to open font file for write: %s", path);
        break;
      }

      fontUpload.valid = true;
      LOG_DBG("WEB", "Font upload started: %s -> %s", filename.c_str(), path);
      break;
    }

    case UPLOAD_FILE_WRITE: {
      if (!fontUpload.valid) break;
      esp_task_wdt_reset();

      // Validate magic bytes on first chunk only
      if (!fontUpload.magicChecked && upload.currentSize >= 8) {
        if (memcmp(upload.buf, "CPFONT\0\0", 8) != 0) {
          LOG_ERR("WEB", "Invalid .cpfont magic bytes");
          fontUpload.valid = false;
          break;
        }
        fontUpload.magicChecked = true;
      }

      // Buffer writes for efficiency
      size_t remaining = upload.currentSize;
      const uint8_t* src = upload.buf;
      while (remaining > 0) {
        size_t space = FontUploadState::BUFFER_SIZE - fontUpload.bufferPos;
        size_t chunk = (remaining < space) ? remaining : space;
        memcpy(fontUpload.buffer.get() + fontUpload.bufferPos, src, chunk);
        fontUpload.bufferPos += chunk;
        src += chunk;
        remaining -= chunk;

        if (fontUpload.bufferPos >= FontUploadState::BUFFER_SIZE) {
          fontUpload.file.write(fontUpload.buffer.get(), fontUpload.bufferPos);
          fontUpload.bytesWritten += fontUpload.bufferPos;
          fontUpload.bufferPos = 0;
          esp_task_wdt_reset();
        }
      }
      break;
    }

    case UPLOAD_FILE_END: {
      // Flush remaining buffer
      if (fontUpload.valid && fontUpload.bufferPos > 0) {
        fontUpload.file.write(fontUpload.buffer.get(), fontUpload.bufferPos);
        fontUpload.bytesWritten += fontUpload.bufferPos;
        fontUpload.bufferPos = 0;
      }
      if (fontUpload.file.isOpen()) {
        fontUpload.file.close();
      }

      if (!fontUpload.valid && !fontUpload.filePath.empty()) {
        Storage.remove(fontUpload.filePath.c_str());
      }
      fontUpload.buffer.reset();

      LOG_DBG("WEB", "Font upload end: valid=%d, %zu bytes", fontUpload.valid, fontUpload.bytesWritten);
      break;
    }

    case UPLOAD_FILE_ABORTED: {
      if (fontUpload.file) {
        fontUpload.file.close();
      }
      if (!fontUpload.filePath.empty()) {
        Storage.remove(fontUpload.filePath.c_str());
      }
      fontUpload.buffer.reset();
      fontUpload.valid = false;
      LOG_DBG("WEB", "Font upload aborted");
      break;
    }
  }
}

void CrossPointWebServer::handleFontUpload() {
  if (fontUpload.valid) {
    sdFontSystem.markRegistryDirty();
    server->send(200, "application/json", "{\"ok\":true}");
    LOG_DBG("WEB", "Font upload complete: %s", fontUpload.filePath.c_str());
  } else {
    server->send(400, "application/json", "{\"error\":\"Invalid .cpfont file\"}");
  }
}

void CrossPointWebServer::handleFontDelete() {
  String body = server->arg("plain");
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);

  if (err || !doc["family"].is<const char*>()) {
    server->send(400, "application/json", "{\"error\":\"Invalid request\"}");
    return;
  }

  const char* familyName = doc["family"];
  FontInstaller installer(sdFontSystem.registry());
  auto result = installer.deleteFamily(familyName);

  if (result == FontInstaller::Error::OK) {
    sdFontSystem.markRegistryDirty();
    server->send(200, "application/json", "{\"ok\":true}");
    LOG_DBG("WEB", "Deleted font family: %s", familyName);
  } else {
    server->send(500, "application/json", "{\"error\":\"Delete failed\"}");
    LOG_ERR("WEB", "Failed to delete font family: %s", familyName);
  }
}
