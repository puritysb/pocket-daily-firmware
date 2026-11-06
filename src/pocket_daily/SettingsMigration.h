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

// A settings file without either fork marker was written by stock CrossPoint:
// this is Pocket Daily's first boot after an install from CrossPoint. The fork
// always writes kLongPressOrderKey, so the first load's resave ends the state.
constexpr bool writtenByStockCrossPoint(const bool hasStartupApp, const bool hasOrderMarker) {
  return !hasStartupApp && !hasOrderMarker;
}

// Stock 2.0 "lineHeight" (100, 110, 120, 130, 145, 160, 175 %) to the fork's
// Tight 0 / Normal 1 / Wide 2 / Extra Wide 3. Stock maps the fork's four steps
// to 110/120/130/145, so this is its inverse; the outer steps fold inward.
constexpr uint8_t lineSpacingFromStockLineHeight(const uint8_t lineHeight) {
  switch (lineHeight) {
    case 0:
    case 1:
      return 0;
    case 2:
      return 1;
    case 3:
      return 2;
    default:
      return 3;
  }
}

// Stock 2.0 removed the built-in serif and selects it as the SD family
// "NotoSerif". Pocket Daily still ships the built-in serif.
constexpr const char* kStockSerifFamily = "NotoSerif";

}  // namespace PocketDaily::SettingsMigration
