#include "HttpDownloader.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <base64.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <strings.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>

#include "HttpOrigin.h"

namespace {
// RX holds the response headers. 4096 fits real OPDS servers; GitHub's release
// CDN sends more and logs HTTP_HEADER "Buffer length is small", but that's
// non-fatal: the headers we read (Location, Content-Length) come first and
// survive. Smaller keeps contiguous heap free while WiFi and TLS are up. TX
// only carries our GET; the body streams in READ_CHUNK pieces.
constexpr int HTTP_RX_BUF = 4096;
// Surface Feed/Outbox/OTA responses are LAN HTTP with compact headers. Keeping
// the public-HTTPS 4 KiB buffer on the no-PSRAM X3 made a second endpoint fail
// immediately whenever the first attempt fragmented the largest heap block.
constexpr int LOCAL_HTTP_RX_BUF = 1024;
constexpr int HTTP_TX_BUF = 1024;
// Per-socket-op timeout. Some OPDS download endpoints are slow to send headers
// (>15s) and chunked catalogs stall mid-body, so 15s killed them. 60s gives
// slow servers room. esp_http_client's timeout_ms is uint32, so unlike Arduino
// HTTPClient's uint16 setTimeout it doesn't silently truncate.
constexpr int HTTP_TIMEOUT_MS = 60000;
// A LAN daemon on the same subnet either answers in well under a second or is
// not there. Charging it the public-internet budget made a single Sync press
// block the loop for 61 s (two candidates x ~30 s in the device log), during
// which no button is polled and the Face cannot repaint — the user sees a
// frozen "Checking for updates" and cannot even leave. Fail fast instead: two
// candidates now cost at most ~16 s, and every caller already retries.
constexpr int LOCAL_HTTP_TIMEOUT_MS = 8000;
constexpr size_t READ_CHUNK = 2048;
constexpr size_t LOCAL_READ_CHUNK = 1024;
constexpr size_t REDIRECT_LOCATION_MAX = 4096;

struct Sink {
  std::function<bool(const uint8_t*, size_t)> write;  // returns false to abort the transfer
  // Optional one-shot allocation before the read buffer is created. Text
  // sinks use Content-Length to avoid a second std::string growth after the
  // first 1 KiB LAN read fragments the X3 heap.
  std::function<bool(size_t)> prepare;
  HttpDownloader::ProgressCallback progress;
  const bool* cancelFlag = nullptr;
  // Stream a 401/403 body to write() too (OPDS authentication documents).
  bool streamUnauthorizedBody = false;
  size_t total = 0;
  size_t downloaded = 0;
};

bool isRedirect(int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

struct RedirectCapture {
  RedirectCapture() = default;
  RedirectCapture(const RedirectCapture&) = delete;
  RedirectCapture& operator=(const RedirectCapture&) = delete;
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

// Keep ESP-IDF's configuration frame separate from streaming/redirect state.
// The client copies configuration; URL and redirect tap remain caller-owned.
__attribute__((noinline)) esp_http_client_handle_t createHttpClient(const std::string& url, RedirectCapture& redirect,
                                                                    esp_http_client_method_t method) {
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.method = method;
  const bool localHttp = url.rfind("http://", 0) == 0;
  config.buffer_size = localHttp ? LOCAL_HTTP_RX_BUF : HTTP_RX_BUF;
  config.buffer_size_tx = HTTP_TX_BUF;
  config.timeout_ms = localHttp ? LOCAL_HTTP_TIMEOUT_MS : HTTP_TIMEOUT_MS;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.keep_alive_enable = false;
  config.disable_auto_redirect = true;
  config.event_handler = httpEventHandler;
  config.user_data = &redirect;
  return esp_http_client_init(&config);
}

__attribute__((noinline)) void setGetHeaders(esp_http_client_handle_t client, const std::string& username,
                                             const std::string& password,
                                             const HttpDownloader::RequestHeaders* requestHeaders, bool sameOrigin) {
  esp_http_client_set_header(client, "User-Agent", "PocketDaily-ESP32-" CROSSPOINT_VERSION);
  if (sameOrigin && requestHeaders && requestHeaders->items) {
    for (size_t i = 0; i < requestHeaders->count; ++i) {
      const auto& header = requestHeaders->items[i];
      if (header.name && header.name[0] && header.value) esp_http_client_set_header(client, header.name, header.value);
    }
  }
  if (sameOrigin && !username.empty() && !password.empty()) {
    // Preemptive Basic auth, like the prior addHeader; don't wait for a 401.
    const std::string credentials = username + ":" + password;
    const String header = "Basic " + base64::encode(credentials.c_str());
    esp_http_client_set_header(client, "Authorization", header.c_str());
  }
}

std::string resolveRedirectUrl(const std::string& currentUrl, const char* location) {
  if (!location || !*location) return "";
  if (strncmp(location, "http://", 7) == 0 || strncmp(location, "https://", 8) == 0) return location;

  const size_t schemeEnd = currentUrl.find("://");
  if (schemeEnd == std::string::npos) return "";
  if (location[0] == '/' && location[1] == '/') return currentUrl.substr(0, schemeEnd + 1) + location;
  const size_t hostStart = schemeEnd + 3;
  const size_t pathStart = currentUrl.find('/', hostStart);

  if (location[0] == '/') {
    const size_t originEnd = pathStart == std::string::npos ? currentUrl.size() : pathStart;
    return currentUrl.substr(0, originEnd) + location;
  }

  const size_t baseEnd = pathStart == std::string::npos ? currentUrl.size() : currentUrl.rfind('/') + 1;
  return currentUrl.substr(0, baseEnd) + location;
}

HttpDownloader::Failure lastFailureState;
int lastStatusState = 0;
bool lastRedirectedState = false;

void noteFailure(const char* stage, const int code = 0, const int tlsCode = 0) {
  lastFailureState = {stage, code, tlsCode, static_cast<uint32_t>(ESP.getFreeHeap()),
                      static_cast<uint32_t>(ESP.getMaxAllocHeap())};
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
                                     Sink& sink, HttpDownloader::HeaderCapture* headerCapture = nullptr,
                                     const HttpDownloader::RequestHeaders* requestHeaders = nullptr,
                                     HttpDownloader::EffectiveUrlCapture* effectiveUrl = nullptr,
                                     const char* credentialOrigin = nullptr) {
  std::string currentUrl = url;
  lastFailureState = {};
  lastStatusState = 0;
  lastRedirectedState = false;
  const std::string_view origin = credentialOrigin ? std::string_view(credentialOrigin) : std::string_view(url);
  if (!HttpOrigin::allowsRequest(origin, currentUrl)) {
    noteFailure("unsafe URL");
    return HttpDownloader::HTTP_ERROR;
  }

  for (int hop = 0; hop <= 5; ++hop) {
    // GitHub release asset redirects carry long signed Azure URLs. Capture the
    // Location after TLS is established, without reserving a large loop stack/BSS
    // buffer on the memory-tight ESP32.
    RedirectCapture redirect;
    redirect.capture = headerCapture;
    const bool localHttp = currentUrl.rfind("http://", 0) == 0;
    esp_http_client_handle_t client = createHttpClient(currentUrl, redirect, HTTP_METHOD_GET);
    if (!client) {
      LOG_ERR("HTTP", "client init failed");
      noteFailure("init");
      return HttpDownloader::HTTP_ERROR;
    }

    setGetHeaders(client, username, password, requestHeaders, HttpOrigin::same(origin, currentUrl));

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
      int tlsCode = 0;
      int tlsFlags = 0;
      esp_http_client_get_and_clear_last_tls_error(client, &tlsCode, &tlsFlags);
      LOG_ERR("HTTP", "open failed: %s tls=-0x%x flags=0x%x", esp_err_to_name(err), (unsigned)-tlsCode,
              (unsigned)tlsFlags);
      noteFailure("open", err, tlsCode);
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }
    logHeap("after open");

    int64_t contentLength = esp_http_client_fetch_headers(client);
    if (contentLength < 0) {
      LOG_ERR("HTTP", "fetch headers failed");
      noteFailure("headers", static_cast<int>(contentLength));
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }
    const int status = esp_http_client_get_status_code(client);
    lastStatusState = status;
    lastRedirectedState = !HttpOrigin::same(url, currentUrl);
    LOG_DBG("HTTP", "status=%d contentLength=%lld", status, (long long)contentLength);

    if (isRedirect(status)) {
      if (hop == 5) {
        LOG_ERR("HTTP", "too many redirects");
        noteFailure("redirects");
        esp_http_client_cleanup(client);
        return HttpDownloader::HTTP_ERROR;
      }
      if (redirect.oom) {
        noteFailure("redirect oom");
        esp_http_client_cleanup(client);
        return HttpDownloader::HTTP_ERROR;
      }
      if (redirect.truncated || !redirect.location || redirect.location[0] == '\0') {
        LOG_ERR("HTTP", "redirect without usable Location");
        noteFailure("redirect");
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
      if (!HttpOrigin::allowsRequest(currentUrl, nextUrl)) {
        LOG_ERR("HTTP", "invalid or insecure redirect Location");
        noteFailure("redirect");
        return HttpDownloader::HTTP_ERROR;
      }
      LOG_DBG("HTTP", "redirect %d accepted", status);
      currentUrl = std::move(nextUrl);
      continue;
    }

    if (effectiveUrl) snprintf(effectiveUrl->value, sizeof(effectiveUrl->value), "%s", currentUrl.c_str());

    if (status == 304) {
      // Conditional matched (?sig= echo) — bodyless by design, not an error.
      esp_http_client_cleanup(client);
      return HttpDownloader::NOT_MODIFIED;
    }
    // Pull OTA resumes use 206 Partial Content. The sink already owns the
    // append offset and validates the completed image size + MD5, so a partial
    // response is a successful body transfer, not an HTTP failure.
    const bool unauthorized = status == 401 || status == 403;
    if (unauthorized && !sink.streamUnauthorizedBody) {
      LOG_ERR("HTTP", "unauthorized: %d", status);
      noteFailure("status", status);
      esp_http_client_cleanup(client);
      return HttpDownloader::UNAUTHORIZED;
    }
    if (!unauthorized && (status < 200 || status > 299)) {
      LOG_ERR("HTTP", "unexpected status: %d", status);
      noteFailure("status", status);
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }

    // fetch_headers returns 0 for a chunked response (no Content-Length); leave
    // total at 0 so progress stays silent and the size check is skipped.
    sink.total = contentLength > 0 ? static_cast<size_t>(contentLength) : 0;
    if (sink.prepare && sink.total > 0 && !sink.prepare(sink.total)) {
      LOG_ERR("HTTP", "sink prepare failed for %zu bytes", sink.total);
      noteFailure("reserve", static_cast<int>(sink.total));
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }

    const size_t readChunk = localHttp ? LOCAL_READ_CHUNK : READ_CHUNK;
    auto buf = makeUniqueNoThrow<char[]>(readChunk);
    if (!buf) {
      LOG_ERR("HTTP", "OOM: %u byte read buffer", (unsigned)readChunk);
      noteFailure("buffer", static_cast<int>(readChunk));
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }

    while (true) {
      if (sink.cancelFlag && *sink.cancelFlag) {
        esp_http_client_cleanup(client);
        return HttpDownloader::ABORTED;
      }
      const int read = esp_http_client_read(client, buf.get(), readChunk);
      if (read < 0) {
        LOG_ERR("HTTP", "read error after %zu bytes", sink.downloaded);
        noteFailure("read", static_cast<int>(sink.downloaded));
        esp_http_client_cleanup(client);
        return HttpDownloader::HTTP_ERROR;
      }
      if (read == 0) break;  // all data received
      if (!sink.write(reinterpret_cast<const uint8_t*>(buf.get()), read)) {
        noteFailure("write", static_cast<int>(sink.downloaded));
        esp_http_client_cleanup(client);
        return HttpDownloader::FILE_ERROR;
      }
      sink.downloaded += read;
      if (sink.progress && sink.total > 0) sink.progress(sink.downloaded, sink.total);
    }

    // ESP-IDF can leave its internal completion flag false after a clean
    // Connection: close even though the advertised body length was received.
    // Content-Length is the stronger, observable invariant for our daemon.
    const bool complete =
        (sink.total > 0 && sink.downloaded == sink.total) || esp_http_client_is_complete_data_received(client);
    esp_http_client_cleanup(client);
    if (!complete) {
      LOG_ERR("HTTP", "incomplete: got %zu of %zu bytes", sink.downloaded, sink.total);
      noteFailure("incomplete", static_cast<int>(sink.downloaded));
      return HttpDownloader::HTTP_ERROR;
    }
    return unauthorized ? HttpDownloader::UNAUTHORIZED : HttpDownloader::OK;
  }

  return HttpDownloader::HTTP_ERROR;
}
}  // namespace

HttpDownloader::Failure HttpDownloader::lastFailure() { return lastFailureState; }
int HttpDownloader::lastStatus() { return lastStatusState; }
bool HttpDownloader::lastAnswerRedirected() { return lastRedirectedState; }

namespace {
// Bounded header table for the upstream-compatible APIs: no map, no per-header
// heap beyond the caller's own strings (and one Bearer string).
constexpr size_t MAX_EXTRA_HEADERS = 12;
struct HeaderTable {
  HttpDownloader::RequestHeader items[MAX_EXTRA_HEADERS];
  HttpDownloader::RequestHeaders view;
  void add(const char* name, const char* value) {
    if (!name || !name[0] || !value || view.count >= MAX_EXTRA_HEADERS) return;
    items[view.count++] = {name, value};
  }
  HeaderTable() { view.items = items; }
};
}  // namespace

bool HttpDownloader::fetchUrl(const std::string& url, const DataCallback& onData, const FetchOptions& options) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  HeaderTable table;
  if (!options.accept.empty()) table.add("Accept", options.accept.c_str());
  if (!options.acceptLanguage.empty()) table.add("Accept-Language", options.acceptLanguage.c_str());
  // The table keeps the pointer, so the header value lives until the request ends.
  const std::string bearer = options.bearer.empty() ? std::string() : "Bearer " + options.bearer;
  if (!bearer.empty()) table.add("Authorization", bearer.c_str());
  for (const auto& header : options.headers) table.add(header.first.c_str(), header.second.c_str());
  Sink sink;
  sink.write = onData;
  sink.streamUnauthorizedBody = options.captureErrorBody;
  const std::string& user = options.bearer.empty() ? options.username : std::string();
  const std::string& pass = options.bearer.empty() ? options.password : std::string();
  const DownloadError result = runGet(url, user, pass, sink, nullptr, &table.view, nullptr,
                                      options.credentialOrigin.empty() ? nullptr : options.credentialOrigin.c_str());
  if (options.statusOut) *options.statusOut = lastStatusState;
  return result == OK;
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress, const bool* cancelFlag,
                                                             const std::string& username, const std::string& password,
                                                             const std::vector<Header>& headers,
                                                             const bool /*downgradeRedirectsToHttp*/,
                                                             const char* credentialOrigin) {
  HeaderTable table;
  for (const auto& header : headers) table.add(header.first.c_str(), header.second.c_str());
  return downloadToFile(url, destPath, std::move(progress), cancelFlag, username, password, nullptr, &table.view,
                        credentialOrigin);
}

bool HttpDownloader::fetchUrl(const std::string& url, Stream& outContent, const std::string& username,
                              const std::string& password, const RequestHeaders* requestHeaders,
                              const char* credentialOrigin) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  Sink sink;
  sink.write = [&outContent](const uint8_t* data, size_t len) { return outContent.write(data, len) == len; };
  return runGet(url, username, password, sink, nullptr, requestHeaders, nullptr, credentialOrigin) == OK;
}

bool HttpDownloader::fetchUrl(const std::string& url, std::string& outContent, const std::string& username,
                              const std::string& password, const RequestHeaders* requestHeaders,
                              EffectiveUrlCapture* effectiveUrl, const char* credentialOrigin) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  outContent.clear();  // start clean; the sink appends, so don't carry prior content
  Sink sink;
  sink.prepare = [&outContent](size_t total) {
    if (total <= outContent.capacity()) return true;
    constexpr size_t kReserveMargin = 512;
    if ((size_t)ESP.getMaxAllocHeap() < total + 1 + kReserveMargin ||
        (size_t)ESP.getFreeHeap() < total + 1 + READ_CHUNK + kReserveMargin) {
      LOG_ERR("HTTP", "OOM guard: cannot reserve %u bytes, free=%u largest=%u", (unsigned)total,
              (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
      return false;
    }
    outContent.reserve(total);
    return outContent.capacity() >= total;
  };
  sink.write = [&outContent](const uint8_t* data, size_t len) {
    // Body accumulation is the one allocation in this client that grows with
    // the response — and this is a -fno-exceptions build, so a failed string
    // growth is std::terminate -> abort, not a catchable bad_alloc (X4 panic
    // at power-off: heap free 7 KB / largest block 3.9 KB when a full /feed
    // arrived). Pre-check the actual likely capacity growth plus a small heap
    // margin. The previous `(body * 2) + 4096` compared unrelated free-heap
    // headroom against the largest *contiguous* allocation and rejected a
    // 286-byte Feed when the X3 still had a healthy 4.6 KiB block.
    if (outContent.size() + len > outContent.capacity()) {
      const size_t needed = outContent.size() + len + 1;
      const size_t likelyCapacity = std::max(needed, std::max<size_t>(64, outContent.capacity() * 2));
      constexpr size_t kAllocationMargin = 512;
      if ((size_t)ESP.getMaxAllocHeap() < likelyCapacity + kAllocationMargin) {
        LOG_ERR("HTTP", "OOM guard: body at %u bytes, largest block %u", (unsigned)outContent.size(),
                (unsigned)ESP.getMaxAllocHeap());
        return false;
      }
    }
    outContent.append(reinterpret_cast<const char*>(data), len);
    return true;
  };
  return runGet(url, username, password, sink, nullptr, requestHeaders, effectiveUrl, credentialOrigin) == OK;
}

bool HttpDownloader::fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username,
                              const std::string& password, const RequestHeaders* requestHeaders,
                              EffectiveUrlCapture* effectiveUrl, const char* credentialOrigin) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  Sink sink;
  sink.write = onData;
  return runGet(url, username, password, sink, nullptr, requestHeaders, effectiveUrl, credentialOrigin) == OK;
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress, const bool* cancelFlag,
                                                             const std::string& username, const std::string& password,
                                                             HeaderCapture* headerCapture,
                                                             const RequestHeaders* requestHeaders,
                                                             const char* credentialOrigin) {
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

  const DownloadError result =
      runGet(url, username, password, sink, headerCapture, requestHeaders, nullptr, credentialOrigin);
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
                              size_t maxResponseBytes, const RequestHeaders* requestHeaders,
                              EffectiveUrlCapture* effectiveUrl) {
  LOG_DBG("HTTP", "POST: %s (%u bytes)", url.c_str(), (unsigned)bodyLen);
  outResponse.clear();
  std::string currentUrl = url;

  // Surface Outbox must follow the same dual-homed-host redirect as Feed and
  // OTA. Replay the small, idempotency-keyed JSON body on at most two hops;
  // esp_http_client's automatic redirect cannot safely replay a streamed POST.
  for (int hop = 0; hop <= 2; ++hop) {
    RedirectCapture redirect;
    esp_http_client_config_t config = {};
    config.url = currentUrl.c_str();
    config.method = HTTP_METHOD_POST;
    // Outbox posts go to the same LAN daemon as the Feed — same fail-fast budget.
    const bool localHttp = currentUrl.rfind("http://", 0) == 0;
    config.buffer_size = localHttp ? LOCAL_HTTP_RX_BUF : HTTP_RX_BUF;
    config.buffer_size_tx = HTTP_TX_BUF;
    config.timeout_ms = localHttp ? LOCAL_HTTP_TIMEOUT_MS : HTTP_TIMEOUT_MS;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.keep_alive_enable = false;
    config.disable_auto_redirect = true;
    config.event_handler = httpEventHandler;
    config.user_data = &redirect;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
      LOG_ERR("HTTP", "client init failed");
      return false;
    }

    esp_http_client_set_header(client, "User-Agent", "PocketDaily-ESP32-" CROSSPOINT_VERSION);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (requestHeaders && requestHeaders->items) {
      for (size_t i = 0; i < requestHeaders->count; ++i) {
        const auto& header = requestHeaders->items[i];
        if (header.name && header.name[0] && header.value)
          esp_http_client_set_header(client, header.name, header.value);
      }
    }

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
    if (isRedirect(status)) {
      if (hop == 2 || redirect.oom || redirect.truncated || !redirect.location || !redirect.location[0]) {
        LOG_ERR("HTTP", "POST redirect unusable");
        esp_http_client_cleanup(client);
        return false;
      }
      std::string nextUrl = resolveRedirectUrl(currentUrl, redirect.location);
      esp_http_client_cleanup(client);
      if (nextUrl.empty()) return false;
      currentUrl = std::move(nextUrl);
      outResponse.clear();
      continue;
    }

    if (effectiveUrl) snprintf(effectiveUrl->value, sizeof(effectiveUrl->value), "%s", currentUrl.c_str());
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
      if (outResponse.size() + (size_t)read > outResponse.capacity()) {
        const size_t worst = (outResponse.size() + read) * 2 + 4096;
        if ((size_t)ESP.getMaxAllocHeap() < worst) {
          LOG_ERR("HTTP", "OOM guard: response at %u bytes, largest block %u", (unsigned)outResponse.size(),
                  (unsigned)ESP.getMaxAllocHeap());
          esp_http_client_cleanup(client);
          return false;
        }
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
  return false;
}

namespace {
// Single-attempt POST used by postForm/postToFile. The response streams to
// onData; no redirects (the endpoints answer directly).
bool runPost(const std::string& url, const std::string& body, const char* contentType,
             const std::function<bool(const char*, int)>& onData, const bool* cancelFlag, int* statusOut,
             size_t* contentLengthOut) {
  lastStatusState = 0;
  lastRedirectedState = false;
  if (!HttpOrigin::allowsRequest(url, url)) return false;
  RedirectCapture redirect;
  esp_http_client_handle_t client = createHttpClient(url, redirect, HTTP_METHOD_POST);
  if (!client) return false;
  esp_http_client_set_header(client, "User-Agent", "PocketDaily-ESP32-" CROSSPOINT_VERSION);
  esp_http_client_set_header(client, "Content-Type", contentType);
  bool ok = esp_http_client_open(client, static_cast<int>(body.size())) == ESP_OK;
  if (ok && !body.empty())
    ok = esp_http_client_write(client, body.data(), static_cast<int>(body.size())) == static_cast<int>(body.size());
  const int64_t length = ok ? esp_http_client_fetch_headers(client) : -1;
  ok = ok && length >= 0;
  const int status = ok ? esp_http_client_get_status_code(client) : 0;
  lastStatusState = status;
  if (statusOut) *statusOut = status;
  if (contentLengthOut) *contentLengthOut = length > 0 ? static_cast<size_t>(length) : 0;
  if (ok) {
    char buf[512];  // bounded stack chunk; the body streams to the sink
    while (true) {
      if (cancelFlag && *cancelFlag) {
        ok = false;
        break;
      }
      const int read = esp_http_client_read(client, buf, sizeof(buf));
      if (read < 0) {
        ok = false;
        break;
      }
      if (read == 0) break;
      if (!onData(buf, read)) {
        ok = false;
        break;
      }
    }
  }
  esp_http_client_cleanup(client);
  return ok;
}
}  // namespace

bool HttpDownloader::postForm(const std::string& url, const std::string& formBody, std::string& outResponse,
                              int* statusOut) {
  static constexpr size_t MAX_RESPONSE = 4096;
  outResponse.clear();
  int status = 0;
  const bool ok = runPost(
      url, formBody, "application/x-www-form-urlencoded",
      [&outResponse](const char* data, const int len) {
        const size_t take =
            std::min(static_cast<size_t>(len), MAX_RESPONSE - std::min(MAX_RESPONSE, outResponse.size()));
        if (take > 0 && (size_t)ESP.getMaxAllocHeap() < outResponse.size() + take + 512) return false;
        outResponse.append(data, take);
        return true;
      },
      nullptr, &status, nullptr);
  if (statusOut) *statusOut = status;
  return ok && status >= 200 && status < 300;
}

HttpDownloader::DownloadError HttpDownloader::postToFile(const std::string& url, const std::string& body,
                                                         const std::string& contentType, const std::string& destPath,
                                                         ProgressCallback progress, const bool* cancelFlag) {
  const std::string partPath = destPath + ".part";
  if (Storage.exists(partPath.c_str())) Storage.remove(partPath.c_str());
  HalFile file;
  if (!Storage.openFileForWrite("HTTP", partPath.c_str(), file)) return FILE_ERROR;
  size_t written = 0;
  size_t total = 0;
  int status = 0;
  bool writeFailed = false;
  const bool ok = runPost(
      url, body, contentType.c_str(),
      [&](const char* data, const int len) {
        if (file.write(reinterpret_cast<const uint8_t*>(data), len) != static_cast<size_t>(len)) {
          writeFailed = true;
          return false;
        }
        written += len;
        if (progress && total > 0) progress(written, total);
        return true;
      },
      cancelFlag, &status, &total);
  file.close();
  if (!ok || status < 200 || status > 299 || written == 0) {
    Storage.remove(partPath.c_str());
    if (cancelFlag && *cancelFlag) return ABORTED;
    if (writeFailed) return FILE_ERROR;
    return (status == 401 || status == 403) ? UNAUTHORIZED : HTTP_ERROR;
  }
  if (Storage.exists(destPath.c_str())) Storage.remove(destPath.c_str());
  if (!Storage.rename(partPath.c_str(), destPath.c_str())) return FILE_ERROR;
  return OK;
}

namespace {
struct ResponseHeaderTap {
  std::vector<HttpDownloader::Header>* out = nullptr;
};

esp_err_t requestEventHandler(esp_http_client_event_t* event) {
  if (event->event_id != HTTP_EVENT_ON_HEADER || !event->header_key || !event->header_value) return ESP_OK;
  auto* tap = static_cast<ResponseHeaderTap*>(event->user_data);
  if (!tap || !tap->out || tap->out->size() >= HttpDownloader::MAX_RESPONSE_HEADERS) return ESP_OK;
  // Bounded per value; plugin auth reads short headers (Location, tokens).
  constexpr size_t MAX_VALUE = 512;
  const size_t len = strnlen(event->header_value, MAX_VALUE);
  if (ESP.getMaxAllocHeap() < len + 1024) return ESP_OK;
  tap->out->emplace_back(event->header_key, std::string(event->header_value, len));
  return ESP_OK;
}

esp_http_client_method_t methodFromName(const char* method) {
  if (!method || strcasecmp(method, "GET") == 0) return HTTP_METHOD_GET;
  if (strcasecmp(method, "POST") == 0) return HTTP_METHOD_POST;
  if (strcasecmp(method, "PUT") == 0) return HTTP_METHOD_PUT;
  if (strcasecmp(method, "PATCH") == 0) return HTTP_METHOD_PATCH;
  if (strcasecmp(method, "DELETE") == 0) return HTTP_METHOD_DELETE;
  if (strcasecmp(method, "HEAD") == 0) return HTTP_METHOD_HEAD;
  return HTTP_METHOD_MAX;
}
}  // namespace

int HttpDownloader::request(const std::string& url, const RequestOptions& options, const DataCallback& onData) {
  lastStatusState = 0;
  lastRedirectedState = false;
  const esp_http_client_method_t method = methodFromName(options.method);
  if (method == HTTP_METHOD_MAX) {
    LOG_ERR("HTTP", "unsupported method: %s", options.method);
    return -1;
  }
  if (!HttpOrigin::allowsRequest(url, url)) return -1;
  if (options.responseHeaders) {
    options.responseHeaders->clear();
    options.responseHeaders->reserve(8);
  }
  ResponseHeaderTap tap{options.responseHeaders};
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.method = method;
  const bool localHttp = url.rfind("http://", 0) == 0;
  config.buffer_size = localHttp ? LOCAL_HTTP_RX_BUF : HTTP_RX_BUF;
  config.buffer_size_tx = HTTP_TX_BUF;
  config.timeout_ms = 30000;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.keep_alive_enable = false;
  config.disable_auto_redirect = true;
  config.event_handler = requestEventHandler;
  config.user_data = &tap;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) return -1;
  esp_http_client_set_header(client, "User-Agent", "PocketDaily-ESP32-" CROSSPOINT_VERSION);
  if (options.headers) {
    for (const auto& header : *options.headers) {
      if (!header.first.empty()) esp_http_client_set_header(client, header.first.c_str(), header.second.c_str());
    }
  }
  const size_t bodySize = options.body ? options.body->size() : 0;
  const auto aborted = [&options] { return options.shouldAbort && *options.shouldAbort && (*options.shouldAbort)(); };
  bool ok = esp_http_client_open(client, static_cast<int>(bodySize)) == ESP_OK;
  if (ok && bodySize > 0) {
    ok = esp_http_client_write(client, options.body->data(), static_cast<int>(bodySize)) == static_cast<int>(bodySize);
  }
  const int64_t length = ok ? esp_http_client_fetch_headers(client) : -1;
  ok = ok && length >= 0;
  const int status = ok ? esp_http_client_get_status_code(client) : -1;
  lastStatusState = status;
  if (ok) {
    uint8_t buf[512];  // bounded stack chunk; the body streams to the sink
    while (true) {
      if (aborted()) {
        ok = false;
        break;
      }
      const int read = esp_http_client_read(client, reinterpret_cast<char*>(buf), sizeof(buf));
      if (read < 0) {
        ok = false;
        break;
      }
      if (read == 0) break;
      if (!onData(buf, static_cast<size_t>(read))) {
        ok = false;
        break;
      }
    }
    if (ok && !esp_http_client_is_complete_data_received(client)) ok = false;
  }
  esp_http_client_cleanup(client);
  if (!ok) {
    LOG_ERR("HTTP", "request failed: %s status=%d", options.method, status);
    return -1;
  }
  return status;
}
