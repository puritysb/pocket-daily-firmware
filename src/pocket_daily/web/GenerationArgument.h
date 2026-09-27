#pragma once

#include <cstdint>
#include <string_view>

namespace PocketDaily::Web {
// Query generations are decimal uint32 values. Parse without strtoul: unsigned
// long is 32 bits on ESP32, where an unchecked overflow saturates at UINT32_MAX.
inline bool parseGeneration(const std::string_view text, uint32_t& value) {
  if (text.empty() || text.size() > 10) return false;
  uint32_t parsed = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    const auto digit = static_cast<uint32_t>(c - '0');
    if (parsed > (UINT32_MAX - digit) / 10U) return false;
    parsed = parsed * 10U + digit;
  }
  value = parsed;
  return true;
}
}  // namespace PocketDaily::Web
