#pragma once

// Stable Pocket Daily product identity.
namespace PocketDaily {

// Developer iteration loop only (`ENABLE_DEV_REMOTE_FLASH` builds, never
// gh_release): marker written before a remote-triggered flash so the next
// boot attempts the saved STA network without a second mode-selection click.
#ifdef ENABLE_DEV_REMOTE_FLASH
inline constexpr const char* DEV_BOOT_FILE_TRANSFER_MARKER = "/.crosspoint/dev-boot-file-transfer";
#endif

inline constexpr const char* PRODUCT_ID = "io.pocketdaily.reader";
inline constexpr const char* PRODUCT_NAME = "Pocket Daily";

}  // namespace PocketDaily
