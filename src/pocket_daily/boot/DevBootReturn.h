#pragma once

#include <cstddef>
#include <cstdint>

#include "pocket_daily/web/Profile.h"

namespace PocketDaily::Boot {
enum class DevBootReturn : uint8_t { None, FileTransferSta, SyncSta, FileTransferMenu, SyncMenu };

inline constexpr char devBootMarker(Web::Profile profile, bool apMode) {
  // A private AP lease is deliberately not persisted across firmware reboot.
  if (Web::isSyncProfile(profile)) return apMode ? 'N' : 'S';
  return apMode ? 'F' : '1';
}

inline constexpr DevBootReturn decodeDevBootMarker(const char* bytes, size_t size) {
  if (!bytes || size != 1) return DevBootReturn::None;
  switch (bytes[0]) {
    case '1':
      return DevBootReturn::FileTransferSta;  // legacy one-byte marker
    case 'S':
      return DevBootReturn::SyncSta;
    case 'F':
      return DevBootReturn::FileTransferMenu;
    case 'N':
      return DevBootReturn::SyncMenu;
    default:
      return DevBootReturn::None;
  }
}
}  // namespace PocketDaily::Boot
