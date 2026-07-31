#include "HttpDownloader.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <base64.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <strings.h>

#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>

namespace {
// RX holds the response headers. 4096 fits real OPDS servers; GitHub's release
// CDN sends more and logs HTTP_HEADER "Buffer length is small", but that's
// non-fatal: the headers we read (Location, Content-Length) come first and
// survive. Smaller keeps contiguous heap free while WiFi and TLS are up. TX
// only carries our GET; the body streams in READ_CHUNK pieces.
constexpr int HTTP_RX_BUF = 4096;
constexpr int HTTP_TX_BUF = 1024;
// Per-socket-op timeout. Some OPDS download endpoints are slow to send headers
// (>15s) and chunked catalogs stall mid-body, so 15s killed them. 60s gives
// slow servers room. esp_http_client's timeout_ms is uint32, so unlike Arduino
// HTTPClient's uint16 setTimeout it doesn't silently truncate.
constexpr int HTTP_TIMEOUT_MS = 60000;
constexpr size_t READ_CHUNK = 2048;
constexpr size_t REDIRECT_LOCATION_MAX = 4096;

struct Sink {
  std::function<bool(const uint8_t*, size_t)> write;  // returns false to abort the transfer
  HttpDownloader::ProgressCallback progress;
  bool* cancelFlag = nullptr;
  size_t total = 0;
  size_t downloaded = 0;
};

bool isRedirect(int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

struct RedirectCapture {
  char* location = nullptr;
  bool truncated = false;
  bool oom = false;
  HttpDownloader::HeaderCapture* capture = nullptr;  // optional named-header tap

  ~RedirectCapture() { free(location); }
};

esp_err_t httpEventHandler(esp_http_client_event_t* event) {
  if (event->event_id != HTTP_EVENT_ON_HEADER || !event->header_key || !event->header_value) return ESP_OK;
  auto* redirect = static_cast<RedirectCapture*>(event->user_data);
  if (!redirect) return ESP_OK;
  if (redirect->capture && redirect->capture->name && strcasecmp(event->header_key, redirect->capture->name) == 0) {
    snprintf(redirect->capture->value, sizeof(redirect->capture->value), "%s", event->header_value);
  }
  if (strcasecmp(event->header_key, "Location") != 0) return ESP_OK;

  const size_t len = strlen(event->header_value);
  if (len >= REDIRECT_LOCATION_MAX) {
    redirect->truncated = true;
    LOG_ERR("HTTP", "redirect Location too long: %zu bytes", len);
    return ESP_OK;
  }
  free(redirect->location);
  redirect->location = static_cast<char*>(malloc(len + 1));
  if (!redirect->location) {
    redirect->oom = true;
    LOG_ERR("HTTP", "OOM: %zu byte redirect Location", len + 1);
    return ESP_OK;
  }
  memcpy(redirect->location, event->header_value, len + 1);
  return ESP_OK;
}

std::string resolveRedirectUrl(const std::string& currentUrl, const char* location) {
  if (!location || !*location) return "";
  if (strncmp(location, "http://", 7) == 0 || strncmp(location, "https://", 8) == 0) return location;

  const size_t schemeEnd = currentUrl.find("://");
  if (schemeEnd == std::string::npos) return "";
  const size_t hostStart = schemeEnd + 3;
  const size_t pathStart = currentUrl.find('/', hostStart);

  if (location[0] == '/') {
    const size_t originEnd = pathStart == std::string::npos ? currentUrl.size() : pathStart;
    return currentUrl.substr(0, originEnd) + location;
  }

  const size_t baseEnd = pathStart == std::string::npos ? currentUrl.size() : currentUrl.rfind('/') + 1;
  return currentUrl.substr(0, baseEnd) + location;
}

void logHeap(const char* stage) {
  LOG_DBG("HTTP", "%s heap free=%u largest=%u min=%u", stage, (unsigned)ESP.getFreeHeap(),
          (unsigned)ESP.getMaxAllocHeap(), (unsigned)ESP.getMinFreeHeap());
}

// Streams a GET body through sink.write in READ_CHUNK pieces. Uses the manual
// open/fetch_headers/read path rather than esp_http_client_perform(): perform()
// pushes the whole body through an event callback and reports a chunked body
// that ends early as ESP_ERR_HTTP_INCOMPLETE_DATA, whereas the read loop streams
// large/slow files and surfaces a short read directly.
HttpDownloader::DownloadError runGet(const std::string& url, const std::string& username, const std::string& password,
                                     Sink& sink, HttpDownloader::HeaderCapture* headerCapture = nullptr) {
  std::string currentUrl = url;

  for (int hop = 0; hop <= 5; ++hop) {
    // GitHub release asset redirects carry long signed Azure URLs. Capture the
    // Location after TLS is established, without reserving a large loop stack/BSS
    // buffer on the memory-tight ESP32.
    RedirectCapture redirect;
    redirect.capture = headerCapture;
    esp_http_client_config_t config = {};
    config.url = currentUrl.c_str();
    config.buffer_size = HTTP_RX_BUF;
    config.buffer_size_tx = HTTP_TX_BUF;
    config.timeout_ms = HTTP_TIMEOUT_MS;
    // Verify HTTPS against the bundled CA roots. This build has esp-tls
    // CONFIG_ESP_TLS_INSECURE off, so an unverified TLS handshake can't be set
    // up at all; the model is public servers over verified https and local
    // servers over plain http (esp_http_client picks the transport from the URL
    // scheme, so http:// needs no cert config). The prior setInsecure() worked
    // only because Arduino's ssl_client drives mbedtls directly.
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.keep_alive_enable = false;
    config.disable_auto_redirect = true;
    config.event_handler = httpEventHandler;
    config.user_data = &redirect;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
      LOG_ERR("HTTP", "client init failed");
      return HttpDownloader::HTTP_ERROR;
    }

    esp_http_client_set_header(client, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
    if (!username.empty() && !password.empty()) {
      // Preemptive Basic auth, like the prior addHeader; don't wait for a 401.
      const std::string credentials = username + ":" + password;
      const String header = "Basic " + base64::encode(credentials.c_str());
      esp_http_client_set_header(client, "Authorization", header.c_str());
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
      LOG_ERR("HTTP", "open failed: %s", esp_err_to_name(err));
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }
    logHeap("after open");

    int64_t contentLength = esp_http_client_fetch_headers(client);
    if (contentLength < 0) {
      LOG_ERR("HTTP", "fetch headers failed");
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }
    const int status = esp_http_client_get_status_code(client);
    LOG_DBG("HTTP", "status=%d contentLength=%lld", status, (long long)contentLength);

    if (isRedirect(status)) {
      if (hop == 5) {
        LOG_ERR("HTTP", "too many redirects");
        esp_http_client_cleanup(client);
        return HttpDownloader::HTTP_ERROR;
      }
      if (redirect.oom) {
        esp_http_client_cleanup(client);
        return HttpDownloader::HTTP_ERROR;
      }
      if (redirect.truncated || !redirect.location || redirect.location[0] == '\0') {
        LOG_ERR("HTTP", "redirect without usable Location");
        esp_http_client_cleanup(client);
        return HttpDownloader::HTTP_ERROR;
      }
      const size_t redirectLen = strlen(redirect.location);
      LOG_DBG("HTTP", "redirect %d Location len=%u", status, (unsigned)redirectLen);
      // Drop the current TLS session before allocating/copying the long signed
      // GitHub release-asset URL. On ESP32-C3, keeping both live can leave only
      // a few hundred bytes free and make the next allocation fail.
      esp_http_client_cleanup(client);
      logHeap("after redirect cleanup");
      std::string nextUrl = resolveRedirectUrl(currentUrl, redirect.location);
      if (nextUrl.empty()) {
        LOG_ERR("HTTP", "invalid redirect Location");
        return HttpDownloader::HTTP_ERROR;
      }
      LOG_DBG("HTTP", "redirect %d -> %s", status, nextUrl.c_str());
      currentUrl = std::move(nextUrl);
      continue;
    }

    if (status == 304) {
      // Conditional matched (?sig= echo) — bodyless by design, not an error.
      esp_http_client_cleanup(client);
      return HttpDownloader::NOT_MODIFIED;
    }
    if (status != 200) {
      LOG_ERR("HTTP", "unexpected status: %d", status);
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }

    // fetch_headers returns 0 for a chunked response (no Content-Length); leave
    // total at 0 so progress stays silent and the size check is skipped.
    sink.total = contentLength > 0 ? static_cast<size_t>(contentLength) : 0;

    auto buf = makeUniqueNoThrow<char[]>(READ_CHUNK);
    if (!buf) {
      LOG_ERR("HTTP", "OOM: %u byte read buffer", (unsigned)READ_CHUNK);
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }

    while (true) {
      if (sink.cancelFlag && *sink.cancelFlag) {
        esp_http_client_cleanup(client);
        return HttpDownloader::ABORTED;
      }
      const int read = esp_http_client_read(client, buf.get(), READ_CHUNK);
      if (read < 0) {
        LOG_ERR("HTTP", "read error after %zu bytes", sink.downloaded);
        esp_http_client_cleanup(client);
        return HttpDownloader::HTTP_ERROR;
      }
      if (read == 0) break;  // all data received
      if (!sink.write(reinterpret_cast<const uint8_t*>(buf.get()), read)) {
        esp_http_client_cleanup(client);
        return HttpDownloader::FILE_ERROR;
      }
      sink.downloaded += read;
      if (sink.progress && sink.total > 0) sink.progress(sink.downloaded, sink.total);
    }

    const bool complete = esp_http_client_is_complete_data_received(client);
    esp_http_client_cleanup(client);
    if (!complete) {
      LOG_ERR("HTTP", "incomplete: got %zu of %zu bytes", sink.downloaded, sink.total);
      return HttpDownloader::HTTP_ERROR;
    }
    return HttpDownloader::OK;
  }

  return HttpDownloader::HTTP_ERROR;
}
}  // namespace

bool HttpDownloader::fetchUrl(const std::string& url, Stream& outContent, const std::string& username,
                              const std::string& password) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  Sink sink;
  sink.write = [&outContent](const uint8_t* data, size_t len) { return outContent.write(data, len) == len; };
  return runGet(url, username, password, sink) == OK;
}

bool HttpDownloader::fetchUrl(const std::string& url, std::string& outContent, const std::string& username,
                              const std::string& password) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  outContent.clear();  // start clean; the sink appends, so don't carry prior content
  Sink sink;
  sink.write = [&outContent](const uint8_t* data, size_t len) {
    outContent.append(reinterpret_cast<const char*>(data), len);
    return true;
  };
  return runGet(url, username, password, sink) == OK;
}

bool HttpDownloader::fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username,
                              const std::string& password) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  Sink sink;
  sink.write = onData;
  return runGet(url, username, password, sink) == OK;
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress, bool* cancelFlag,
                                                             const std::string& username, const std::string& password,
                                                             HeaderCapture* headerCapture) {
  LOG_DBG("HTTP", "Downloading: %s -> %s", url.c_str(), destPath.c_str());

  if (Storage.exists(destPath.c_str())) {
    Storage.remove(destPath.c_str());
  }
  HalFile file;
  if (!Storage.openFileForWrite("HTTP", destPath.c_str(), file)) {
    LOG_ERR("HTTP", "Failed to open file for writing");
    return FILE_ERROR;
  }

  Sink sink;
  sink.progress = std::move(progress);
  sink.cancelFlag = cancelFlag;
  sink.write = [&file](const uint8_t* data, size_t len) { return file.write(data, len) == len; };

  const DownloadError result = runGet(url, username, password, sink, headerCapture);
  // Close before any remove() on the same path; DESTRUCTOR_CLOSES_FILE would
  // otherwise close only after the remove.
  file.close();

  if (result != OK) {
    Storage.remove(destPath.c_str());
    return result;
  }
  if (sink.downloaded == 0) {
    LOG_ERR("HTTP", "no data received");
    Storage.remove(destPath.c_str());
    return HTTP_ERROR;
  }
  LOG_DBG("HTTP", "Downloaded %zu bytes", sink.downloaded);
  return OK;
}

bool HttpDownloader::postJson(const std::string& url, const char* body, size_t bodyLen, std::string& outResponse,
                              size_t maxResponseBytes) {
  LOG_DBG("HTTP", "POST: %s (%u bytes)", url.c_str(), (unsigned)bodyLen);
  outResponse.clear();

  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.method = HTTP_METHOD_POST;
  config.buffer_size = HTTP_RX_BUF;
  config.buffer_size_tx = HTTP_TX_BUF;
  config.timeout_ms = HTTP_TIMEOUT_MS;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.keep_alive_enable = false;
  config.disable_auto_redirect = true;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    LOG_ERR("HTTP", "client init failed");
    return false;
  }

  esp_http_client_set_header(client, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  esp_http_client_set_header(client, "Content-Type", "application/json");

  esp_err_t err = esp_http_client_open(client, bodyLen);
  if (err != ESP_OK) {
    LOG_ERR("HTTP", "open failed: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return false;
  }
  if (bodyLen && esp_http_client_write(client, body, bodyLen) != (int)bodyLen) {
    LOG_ERR("HTTP", "body write failed");
    esp_http_client_cleanup(client);
    return false;
  }

  if (esp_http_client_fetch_headers(client) < 0) {
    LOG_ERR("HTTP", "fetch headers failed");
    esp_http_client_cleanup(client);
    return false;
  }
  const int status = esp_http_client_get_status_code(client);

  char buf[512];
  while (true) {
    const int read = esp_http_client_read(client, buf, sizeof(buf));
    if (read < 0) {
      LOG_ERR("HTTP", "read error after %u bytes", (unsigned)outResponse.size());
      esp_http_client_cleanup(client);
      return false;
    }
    if (read == 0) break;
    if (outResponse.size() + read > maxResponseBytes) {
      LOG_ERR("HTTP", "response exceeds %u bytes — aborted", (unsigned)maxResponseBytes);
      esp_http_client_cleanup(client);
      return false;
    }
    outResponse.append(buf, read);
  }
  esp_http_client_cleanup(client);

  if (status != 200) {
    LOG_ERR("HTTP", "POST status %d: %.120s", status, outResponse.c_str());
    return false;
  }
  return true;
}
