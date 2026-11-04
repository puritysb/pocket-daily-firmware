#pragma once

#include <cstdint>
#include <string_view>

// Allocation-free origin checks; views borrow the caller's URL for this request.
namespace HttpOrigin {
inline char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; }
inline bool equal(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (lower(a[i]) != lower(b[i])) return false;
  return true;
}
struct Origin {
  std::string_view host;
  uint16_t port = 0;  // zero marks an invalid origin
  bool secure = false;
};
inline Origin parse(std::string_view url, bool configuredServer = false) {
  for (unsigned char c : url)
    if (c <= 0x20 || c == 0x7f || c == '\\') return {};
  const auto schemeEnd = url.find("://");
  bool secure = false;
  if (schemeEnd == std::string_view::npos) {
    // Saved OPDS servers historically accept a bare host, meaning HTTP.
    if (!configuredServer) return {};
  } else {
    const auto scheme = url.substr(0, schemeEnd);
    secure = equal(scheme, "https");
    if (!secure && !equal(scheme, "http")) return {};
    url.remove_prefix(schemeEnd + 3);
  }
  const auto authority = url.substr(0, url.find_first_of("/?#"));
  if (authority.empty() || authority.find('@') != std::string_view::npos) return {};
  std::string_view host, port;
  if (authority.front() == '[') {
    const auto end = authority.find(']');
    if (end == std::string_view::npos || end == 1) return {};
    host = authority.substr(0, end + 1);
    if (host.find(':') == std::string_view::npos) return {};
    for (char c : host.substr(1, host.size() - 2)) {
      if (!((lower(c) >= 'a' && lower(c) <= 'f') || (c >= '0' && c <= '9') || c == ':' || c == '.')) return {};
    }
    if (end + 1 < authority.size()) {
      if (authority[end + 1] != ':') return {};
      port = authority.substr(end + 2);
      if (port.empty()) return {};
    }
  } else {
    const auto colon = authority.find(':');
    host = authority.substr(0, colon);
    if (host.empty() || host.size() > 253) return {};
    for (char c : host) {
      if (!((lower(c) >= 'a' && lower(c) <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_'))
        return {};
    }
    if (colon != std::string_view::npos) {
      port = authority.substr(colon + 1);
      if (port.empty()) return {};
    }
  }
  unsigned number = secure ? 443 : 80;
  if (!port.empty()) {
    number = 0;
    for (char c : port) {
      if (c < '0' || c > '9' || number > 6553) return {};
      number = number * 10 + static_cast<unsigned>(c - '0');
      if (number > 65535) return {};
    }
    if (number == 0) return {};
  }
  return {host, static_cast<uint16_t>(number), secure};
}
[[gnu::noinline]] inline bool same(std::string_view credentialOrigin, std::string_view target) {
  const auto a = parse(credentialOrigin, true), b = parse(target);
  return a.port != 0 && b.port != 0 && a.port == b.port && a.secure == b.secure && equal(a.host, b.host);
}
[[gnu::noinline]] inline bool allowsRequest(std::string_view source, std::string_view target) {
  const auto a = parse(source, true), b = parse(target);
  return a.port != 0 && b.port != 0 && !(a.secure && !b.secure);
}
}  // namespace HttpOrigin
