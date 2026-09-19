#pragma once
#include <cstdint>

namespace PocketDaily::DirectSession {
// Allow the HTTP response to leave before the activity tears down Wi-Fi.
constexpr bool shouldEnd(bool requested, uint32_t requestedAt, uint32_t now) {
  return requested && static_cast<uint32_t>(now - requestedAt) >= 500U;
}
}  // namespace PocketDaily::DirectSession
