#pragma once

#include <cstdint>

// Settings numbering the fork changed and must read back from older files.
namespace PocketDaily::SettingsMigration {

// Long-press Confirm (CrossPointSettings::LONG_PRESS_MENU_FUNCTION). Stock
// CrossPoint stores KOSync 0, Disabled 1, Bookmark 2, Dictionary 3, Reader
// Menu 4. Fork files carried a Bilingual Toggle, removed on 2026-10-31: files
// with kLongPressOrderKey stored it as 5; older fork files (recognised by the
// fork-only "startupApp" key) inserted it at 3 and shifted Dictionary to 4 and
// Reader Menu to 5. A retired Bilingual Toggle reads as Disabled.
constexpr const char* kLongPressOrderKey = "longPressMenuOrder";
constexpr uint8_t kLongPressOrder = 1;
constexpr uint8_t kLongPressDisabled = 1;

constexpr uint8_t longPressFromFile(const uint8_t stored, const bool writtenByFork, const bool hasOrderMarker) {
  if (hasOrderMarker) return stored == 5 ? kLongPressDisabled : stored;
  if (!writtenByFork) return stored;
  switch (stored) {
    case 3:
      return kLongPressDisabled;  // retired Bilingual Toggle
    case 4:
      return 3;  // Dictionary
    case 5:
      return 4;  // Reader Menu
    default:
      return stored;
  }
}

}  // namespace PocketDaily::SettingsMigration
