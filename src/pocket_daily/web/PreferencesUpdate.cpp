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
  const auto cover = doc["pocketDailySleepCover"];
  if (!cover.isNull()) {
    if (cover.is<bool>()) {
      parsed.sleepCover = cover.as<bool>() ? 1 : 0;
    } else if (cover.is<int>()) {
      parsed.sleepCover = cover.as<int>() ? 1 : 0;
    } else {
      error = "Invalid pocketDailySleepCover";
      return false;
    }
    parsed.hasSleepCover = true;
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
  out = parsed;
  return true;
}
}  // namespace PocketDaily::Web
