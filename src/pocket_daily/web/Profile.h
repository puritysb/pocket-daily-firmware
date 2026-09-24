#pragma once

#include <cstdint>

namespace PocketDaily::Web {

// Which surface an incarnation of the inherited web server exposes. Pocket
// owns the definition: the modules under src/pocket_daily/web/ gate routes,
// listeners, and watchdog policy on it, and the inherited
// CrossPointWebServer.h aliases it (CrossPointWebServerProfile) so existing
// call sites compile unchanged.
enum class Profile : uint8_t {
  FULL,           // Upstream's complete browser surface (X4, legacy use)
  FILE_TRANSFER,  // Manual File Transfer: browser routes, no WebDAV/discovery
  POCKET_SYNC,    // Pocket's authenticated private AP: minimal route set only
  COMPANION,      // Pocket Sync on shared Wi-Fi: app routes, no browser services
};

inline constexpr bool hasBrowserRoutes(Profile profile) {
  return profile == Profile::FULL || profile == Profile::FILE_TRANSFER;
}

// Dedicated sessions reserve radio/socket memory for verified transfers and
// content presentation. This policy applies on both boards and both bearers;
// extra heap must not silently enable optional services halfway through sync.
inline constexpr bool isSyncProfile(Profile profile) {
  return profile == Profile::COMPANION || profile == Profile::POCKET_SYNC;
}

inline constexpr bool allowsLivePush(Profile profile, bool apMode) { return !isSyncProfile(profile) && !apMode; }

inline constexpr Profile selectProfile(bool privateAp, bool deviceIsX3, bool companion = false) {
  if (privateAp) return Profile::POCKET_SYNC;
  if (companion) return Profile::COMPANION;
  return deviceIsX3 ? Profile::FILE_TRANSFER : Profile::FULL;
}

}  // namespace PocketDaily::Web
