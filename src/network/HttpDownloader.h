#pragma once
#include <HalStorage.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

/**
 * HTTP client utility for fetching content and downloading files. Built on
 * esp_http_client: https is verified against the CA bundle, plain http is
 * used for local servers (transport is chosen from the URL scheme).
 */
class HttpDownloader {
 public:
  // Conservative admission floor; hardware TLS headroom must still be measured.
  static constexpr uint32_t MIN_TLS_FREE_HEAP = 40000;
  static constexpr uint32_t MIN_TLS_MAX_ALLOC = 20000;
  using ProgressCallback = std::function<void(size_t downloaded, size_t total)>;
  // Called with each body chunk as it arrives; return false to abort. Lets a
  // streaming parser consume the response without buffering the whole body.
  using DataCallback = std::function<bool(const uint8_t* data, size_t len)>;

  enum DownloadError {
    OK = 0,
    HTTP_ERROR,
    FILE_ERROR,
    ABORTED,
    // 304 — the server's conditional (e.g. ?sig= echo) matched; no body was
    // sent and nothing was written. Only surfaced by downloadToFile.
    NOT_MODIFIED,
    UNAUTHORIZED,  // 401/403: callers holding a refreshable credential can retry
  };

  // Extra request header (e.g. a per-server custom header or Bearer token).
  // Sent only to the credential origin, like Basic credentials.
  using Header = std::pair<std::string, std::string>;

  /**
   * HTTP status of the last GET/POST on this task, or 0 when it never got a
   * response (DNS, connect, TLS, timeout).
   */
  static int lastStatus();

  /**
   * True when the last answer came from a different origin than the URL asked
   * for, after a redirect. Credentials are withheld from such a hop, so a 401
   * there is not a wrong password.
   */
  static bool lastAnswerRedirected();

  /**
   * Upstream-compatible streaming GET options. `bearer` overrides Basic
   * credentials. `credentialOrigin` (Pocket Daily) names the configured server:
   * credentials and headers go only to that origin; empty means the request URL.
   */
  struct FetchOptions {
    std::string username;
    std::string password;
    std::string bearer;
    std::string accept;
    std::string acceptLanguage;
    std::vector<Header> headers;
    std::string credentialOrigin;
    int* statusOut = nullptr;
    bool captureErrorBody = false;
  };

  static bool fetchUrl(const std::string& url, const DataCallback& onData, const FetchOptions& options);

  /**
   * POST an application/x-www-form-urlencoded body and collect the response
   * (any status, capped at 4 KiB). Returns true on 2xx. No redirects.
   */
  static bool postForm(const std::string& url, const std::string& formBody, std::string& outResponse,
                       int* statusOut = nullptr);

  /**
   * POST a body and stream the response to SD through "<dest>.part". Single
   * attempt, no redirects.
   */
  static DownloadError postToFile(const std::string& url, const std::string& body, const std::string& contentType,
                                  const std::string& destPath, ProgressCallback progress = nullptr,
                                  const bool* cancelFlag = nullptr);

  /**
   * Upstream-compatible download with extra headers. HTTPS is never downgraded:
   * `downgradeRedirectsToHttp` is accepted for source compatibility and ignored,
   * because HttpOrigin rejects HTTPS-to-HTTP transitions (docs/http-credential-origin.md).
   */
  static DownloadError downloadToFile(const std::string& url, const std::string& destPath, ProgressCallback progress,
                                      const bool* cancelFlag, const std::string& username, const std::string& password,
                                      const std::vector<Header>& headers, bool downgradeRedirectsToHttp = false,
                                      const char* credentialOrigin = nullptr);

  // Why the most recent GET on this task failed, for a user-visible reason on
  // screens like the update check (serial logs are not available to users).
  // `stage` is a static string; `code` is the esp_err_t, HTTP status or byte
  // count for that stage; `tlsCode` is the mbedTLS error from a failed open.
  struct Failure {
    const char* stage = nullptr;
    int code = 0;
    int tlsCode = 0;
    uint32_t freeHeap = 0;
    uint32_t largestBlock = 0;
  };
  // Preserve this small diagnostic snapshot across the next request on this
  // task; a reference would expose subsequent writes to lastFailureState.
  // cppcheck-suppress returnByReference
  static Failure lastFailure();

  // Optional single response-header capture (e.g. X-Frame-Sig). The value is
  // snprintf-truncated to the fixed buffer; empty when the header never came.
  struct HeaderCapture {
    const char* name = nullptr;
    char value[48] = {0};
  };

  // Optional final URL after redirects. The fixed buffer is intentionally
  // small: OTA only needs the origin at the beginning, not a copied token or
  // long signed asset query.
  struct EffectiveUrlCapture {
    char value[96] = {0};
  };

  // Optional fixed request headers. Callers own both arrays and strings for
  // the duration of the blocking request. This deliberately avoids maps and
  // heap allocation on the no-PSRAM ESP32-C3.
  struct RequestHeader {
    const char* name = nullptr;
    const char* value = nullptr;
  };
  struct RequestHeaders {
    const RequestHeader* items = nullptr;
    size_t count = 0;
  };

  /**
   * Fetch text content with credentials/custom headers bound to the initial origin.
   * OPDS callers supply their configured server as credentialOrigin, so absolute
   * feed/acquisition links cannot inherit credentials for another origin.
   * HTTPS-to-HTTP transitions fail before a network request.
   */
  static bool fetchUrl(const std::string& url, std::string& outContent, const std::string& username = "",
                       const std::string& password = "", const RequestHeaders* requestHeaders = nullptr,
                       EffectiveUrlCapture* effectiveUrl = nullptr, const char* credentialOrigin = nullptr);

  static bool fetchUrl(const std::string& url, Stream& stream, const std::string& username = "",
                       const std::string& password = "", const RequestHeaders* requestHeaders = nullptr,
                       const char* credentialOrigin = nullptr);

  /**
   * Stream the response body to onData as it arrives, without buffering it.
   */
  static bool fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username = "",
                       const std::string& password = "", const RequestHeaders* requestHeaders = nullptr,
                       EffectiveUrlCapture* effectiveUrl = nullptr, const char* credentialOrigin = nullptr);

  /**
   * Download a file to the SD card with optional credentials.
   */
  static DownloadError downloadToFile(const std::string& url, const std::string& destPath,
                                      ProgressCallback progress = nullptr, const bool* cancelFlag = nullptr,
                                      const std::string& username = "", const std::string& password = "",
                                      HeaderCapture* headerCapture = nullptr,
                                      const RequestHeaders* requestHeaders = nullptr,
                                      const char* credentialOrigin = nullptr);

  // One request with an arbitrary method (plugin manifests declare GET, POST,
  // PUT, PATCH, DELETE), certificate-verified like every other HTTPS call here.
  // No redirect following: a 3xx is returned as its status. Headers go only to
  // the request URL's own origin by construction (no redirects).
  struct RequestOptions {
    const char* method = "GET";
    const std::string* body = nullptr;
    const std::vector<Header>* headers = nullptr;
    // Receives response headers in receive order (bounded: MAX_RESPONSE_HEADERS).
    std::vector<Header>* responseHeaders = nullptr;
    const std::function<bool()>* shouldAbort = nullptr;
  };
  static constexpr size_t MAX_RESPONSE_HEADERS = 24;
  // Returns the HTTP status (error statuses still stream their body), or -1 on
  // transport failure, abort, a sink refusal, or a short body.
  static int request(const std::string& url, const RequestOptions& options, const DataCallback& onData);

  /**
   * POST a JSON body and collect the (bounded) JSON response. Plain-http local
   * daemons and verified https both work (transport picked from the scheme).
   * No redirect handling — intended for the AgentDeck daemon's LAN endpoints.
   */
  static bool postJson(const std::string& url, const char* body, size_t bodyLen, std::string& outResponse,
                       size_t maxResponseBytes = 16384, const RequestHeaders* requestHeaders = nullptr,
                       EffectiveUrlCapture* effectiveUrl = nullptr);
};
