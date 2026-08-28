#pragma once

#include <cstdint>

namespace PocketDaily {
namespace FontPackSync {

inline constexpr char PACKAGE_ID[] = "pocket-sans-world";
inline constexpr char FAMILY_NAME[] = "PocketSansWorld";
inline constexpr char FONT_DIR[] = "/.fonts/PocketSansWorld";
inline constexpr char FONT_PATH[] = "/.fonts/PocketSansWorld/PocketSansWorld_12.cpfont";
inline constexpr char TEMP_PATH[] = "/.fonts/PocketSansWorld/PocketSansWorld_12.download";
inline constexpr char BACKUP_PATH[] = "/.fonts/PocketSansWorld/PocketSansWorld_12.bak";
inline constexpr char STATE_DIR[] = "/pocket-daily/fonts";
inline constexpr char STATE_PATH[] = "/pocket-daily/fonts/pocket-sans-world.state";
inline constexpr uint16_t FORMAT_VERSION = 4;
inline constexpr uint32_t MAX_PACK_BYTES = 14U * 1024U * 1024U;

struct Advert {
  char packageId[32] = {0};
  uint32_t contentVersion = 0;
  uint16_t formatVersion = 0;
  uint32_t size = 0;
  char md5[33] = {0};
  char licenseSpdx[32] = {0};
};

enum class Result : uint8_t { NotAdvertised, Current, Updated, Failed };

// Download and atomically install the broad SD-paged reader font. This is
// called only by foreground Sync because the pack is roughly 11 MB.
Result sync(const char* ip, uint16_t port, const char* token, const char* board, const Advert& advert);

}  // namespace FontPackSync
}  // namespace PocketDaily
