#include "PreferencesUpdate.h"

#include <ArduinoJson.h>

namespace PocketDaily::Web {
namespace {
// Present (non-null) integer within [low, high]; absent is fine.
bool readRange(JsonVariantConst value, const int low, const int high, bool& has, uint8_t& out) {
  if (value.isNull()) return true;
  if (!value.is<int>()) return false;
  const int v = value.as<int>();
  if (v < low || v > high) return false;
  has = true;
  out = static_cast<uint8_t>(v);
  return true;
}

// Present (non-null) boolean, or integer where non-zero means on; absent is fine.
bool readToggle(JsonVariantConst value, bool& has, uint8_t& out) {
  if (value.isNull()) return true;
  if (value.is<bool>()) {
    out = value.as<bool>() ? 1 : 0;
  } else if (value.is<int>()) {
    out = value.as<int>() ? 1 : 0;
  } else {
    return false;
  }
  has = true;
  return true;
}
}  // namespace

bool parsePreferences(const char* json, const size_t length, const PreferenceLimits& limits, PreferencesUpdate& out,
                      const char*& error) {
  error = "Invalid preferences";
  if (!json || length == 0 || length > 512) {
    error = "Missing JSON body";
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, json, length) != DeserializationError::Ok || !doc.is<JsonObjectConst>()) {
    error = "Invalid JSON";
    return false;
  }
  PreferencesUpdate parsed;
  if (!readRange(doc["startupApp"], 0, limits.startupAppCount - 1, parsed.hasStartupApp, parsed.startupApp)) {
    error = "Invalid startupApp";
    return false;
  }
  if (!readToggle(doc["sleepWakeIndicator"], parsed.hasSleepWakeIndicator, parsed.sleepWakeIndicator)) {
    error = "Invalid sleepWakeIndicator";
    return false;
  }
  if (!readToggle(doc["pocketDailySleepCover"], parsed.hasSleepCover, parsed.sleepCover)) {
    error = "Invalid pocketDailySleepCover";
    return false;
  }
  if (!readRange(doc["sleepTimeoutMinutes"], limits.minSleepMinutes, limits.maxSleepMinutes, parsed.hasSleepTimeout,
                 parsed.sleepTimeoutMinutes)) {
    error = "Invalid sleepTimeoutMinutes";
    return false;
  }
  if (!readRange(doc["fontSize"], 0, limits.fontSizeCount - 1, parsed.hasFontSize, parsed.fontSize)) {
    error = "Invalid fontSize";
    return false;
  }
  if (!readRange(doc["sideButtonLayout"], 0, limits.sideButtonLayoutCount - 1, parsed.hasSideButtonLayout,
                 parsed.sideButtonLayout)) {
    error = "Invalid sideButtonLayout";
    return false;
  }
  if (!readToggle(doc["frontButtonFollowOrientation"], parsed.hasFrontButtonFollowOrientation,
                  parsed.frontButtonFollowOrientation)) {
    error = "Invalid frontButtonFollowOrientation";
    return false;
  }
  if (!readRange(doc["orientation"], 0, limits.orientationCount - 1, parsed.hasOrientation, parsed.orientation)) {
    error = "Invalid orientation";
    return false;
  }
  if (!readRange(doc["lineSpacing"], 0, limits.lineSpacingCount - 1, parsed.hasLineSpacing, parsed.lineSpacing)) {
    error = "Invalid lineSpacing";
    return false;
  }
  if (!readRange(doc["screenMargin"], limits.minScreenMargin, limits.maxScreenMargin, parsed.hasScreenMargin,
                 parsed.screenMargin)) {
    error = "Invalid screenMargin";
    return false;
  }
  out = parsed;
  return true;
}
}  // namespace PocketDaily::Web
