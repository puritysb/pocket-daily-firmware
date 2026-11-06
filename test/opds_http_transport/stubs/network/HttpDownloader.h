#pragma once
// Host stub: records the options OpdsHttpTransport hands to the HTTP layer.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

class HttpDownloader {
 public:
  using DataCallback = std::function<bool(const uint8_t* data, size_t len)>;
  using Header = std::pair<std::string, std::string>;
  struct FetchOptions {
    std::string username, password, bearer, accept, acceptLanguage;
    std::vector<Header> headers;
    std::string credentialOrigin;
    int* statusOut = nullptr;
    bool captureErrorBody = false;
  };
  static bool fetchUrl(const std::string& url, const DataCallback&, const FetchOptions& options);
  static bool postForm(const std::string&, const std::string&, std::string&, int* statusOut) {
    if (statusOut) *statusOut = 200;
    return true;
  }
};

namespace http_stub {
inline HttpDownloader::FetchOptions lastOptions;
inline std::string lastUrl;
}  // namespace http_stub

inline bool HttpDownloader::fetchUrl(const std::string& url, const DataCallback&, const FetchOptions& options) {
  http_stub::lastUrl = url;
  http_stub::lastOptions = options;
  if (options.statusOut) *options.statusOut = 200;
  return true;
}
