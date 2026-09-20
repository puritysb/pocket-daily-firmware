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
  FILE_TRANSFER,  // Manual File Transfer: browser routes, no WS push
  POCKET_SYNC,    // Pocket's authenticated private AP: minimal route set only
};

}  // namespace PocketDaily::Web
