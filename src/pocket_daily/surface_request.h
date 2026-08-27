#pragma once

// Must precede headers that pull in lwIP; HttpDownloader's SdFat macros and
// lwIP's ip4_addr declarations otherwise collide in this firmware build.
#include "network/HttpDownloader.h"
#include "pocket_daily/product_identity.h"

namespace PocketDaily {

// Stack-only adapter for AgentDeck Surface Protocol v1 HTTP identity. The
// request is blocking, so all referenced strings remain alive for its full
// duration. No dynamic allocation is introduced on the ESP32-C3.
struct SurfaceRequestHeaders {
  explicit SurfaceRequestHeaders(const char* board)
      : values{
            {"AgentDeck-Surface-Protocol", SURFACE_PROTOCOL},
            {"AgentDeck-Surface-Profile", SURFACE_PROFILE},
            {"AgentDeck-Client-Id", CLIENT_ID},
            {"AgentDeck-Client-Version", CROSSPOINT_VERSION},
            {"AgentDeck-Product-Id", PRODUCT_ID},
            {"AgentDeck-Capabilities", SURFACE_CAPABILITIES},
            {"AgentDeck-Board", board ? board : ""},
            {"AgentDeck-Update-Channel", UPDATE_CHANNEL},
        } {}

  HttpDownloader::RequestHeaders view() const { return {values, sizeof(values) / sizeof(values[0])}; }

 private:
  HttpDownloader::RequestHeader values[8];
};

}  // namespace PocketDaily
