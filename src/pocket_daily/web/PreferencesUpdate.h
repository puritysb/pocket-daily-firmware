#pragma once

#include <cstddef>
#include <cstdint>

// POST /api/pocket/v1/preferences body, validated as a whole before any
// setting changes (no partial application). Pure; host tested.
namespace PocketDaily::Web {
struct PreferenceLimits {
  uint8_t startupAppCount;
  uint8_t minSleepMinutes;
  uint8_t maxSleepMinutes;
  uint8_t fontSizeCount;
  uint8_t sideButtonLayoutCount;
  uint8_t orientationCount = 4;
  uint8_t lineSpacingCount = 4;
  uint8_t minScreenMargin = 5;
  uint8_t maxScreenMargin = 40;
};

struct PreferencesUpdate {
  bool hasOrientation = false;
  uint8_t orientation = 0;
  bool hasLineSpacing = false;
  uint8_t lineSpacing = 0;
  bool hasScreenMargin = false;
  uint8_t screenMargin = 0;

  bool hasStartupApp = false;
  uint8_t startupApp = 0;
  bool hasSleepWakeIndicator = false;
  uint8_t sleepWakeIndicator = 0;
  bool hasSleepCover = false;
  uint8_t sleepCover = 0;
  bool hasSleepTimeout = false;
  uint8_t sleepTimeoutMinutes = 0;
  bool hasFontSize = false;
  uint8_t fontSize = 0;
  bool hasSideButtonLayout = false;
  uint8_t sideButtonLayout = 0;
  bool hasFrontButtonFollowOrientation = false;
  uint8_t frontButtonFollowOrientation = 0;
};

// Integers must be JSON integers (a string is not 0). pocketDailySleepCover and
// sleepWakeIndicator and frontButtonFollowOrientation accept a boolean or an integer (non-zero means
// on). Unknown keys stay ignored for older/newer clients. `error` is a short static reason.
bool parsePreferences(const char* json, size_t length, const PreferenceLimits& limits, PreferencesUpdate& out,
                      const char*& error);
}  // namespace PocketDaily::Web
