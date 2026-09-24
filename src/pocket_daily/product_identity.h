#pragma once

#include <cstring>

// Stable product identity shared by every optional content-source adapter.
// AgentDeck is currently one provider; it is deliberately not the product.
namespace PocketDaily {

// Developer iteration loop only (`ENABLE_DEV_REMOTE_FLASH` builds, never
// gh_release): marker written before a remote-triggered flash so the next
// boot attempts the saved STA network without a second mode-selection click.
#ifdef ENABLE_DEV_REMOTE_FLASH
inline constexpr const char* DEV_BOOT_FILE_TRANSFER_MARKER = "/.crosspoint/dev-boot-file-transfer";
#endif

inline constexpr const char* PRODUCT_ID = "io.pocketdaily.reader";
inline constexpr const char* PRODUCT_NAME = "Pocket Daily";
inline constexpr const char* CLIENT_ID = "io.pocketdaily.reader";
inline constexpr const char* SURFACE_PROTOCOL = "1";
inline constexpr unsigned SURFACE_PROTOCOL_REVISION = 1;
inline constexpr const char* SURFACE_PROFILE = "portable-reader/v1";
// Legacy query value retained for the current AgentDeck baseline. The public
// profile identity is SURFACE_PROFILE and travels in headers/registration.
inline constexpr const char* LEGACY_SURFACE_QUERY = "pocket-reader";
inline constexpr const char* UPDATE_CHANNEL = "stable";
inline constexpr const char* SURFACE_CAPABILITIES =
    "feed.pull,feed.conditional,outbox.push,glance.read,weather.snapshot.read,learning.pack.read,learning.pack.update,"
    "font.pack.read,font.pack.update,"
    "ota.feed,ota.resume-206,device.telemetry";
inline constexpr const char* SURFACE_CAPABILITIES_JSON =
    "[\"feed.pull\",\"feed.conditional\",\"outbox.push\",\"glance.read\",\"weather.snapshot.read\","
    "\"learning.pack.read\",\"learning.pack.update\",\"font.pack.read\",\"font.pack.update\","
    "\"ota.feed\",\"ota.resume-206\",\"device.telemetry\"]";
inline constexpr const char* AGENTDECK_PROVIDER_ID = "agentdeck";

inline bool otaIdentityMatches(const char* productId, const char* board, const char* updateChannel,
                               const char* expectedBoard) {
  return productId && productId[0] && board && board[0] && updateChannel && updateChannel[0] && expectedBoard &&
         expectedBoard[0] && strcmp(productId, PRODUCT_ID) == 0 && strcmp(board, expectedBoard) == 0 &&
         strcmp(updateChannel, UPDATE_CHANNEL) == 0;
}

}  // namespace PocketDaily
