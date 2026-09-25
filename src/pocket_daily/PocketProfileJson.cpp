#include <ArduinoJson.h>

#include <cstring>

#include "PocketProfile.h"

// JSON codec for the profile endpoints (device and host tests). The renderer
// and the host preview need only PocketProfile.cpp, not ArduinoJson.
namespace PocketDaily::DailyProfile {
namespace {
struct Name {
  const char* text;
  uint8_t value;
};
constexpr Name kHomeItems[] = {{"reading", 1}, {"study", 2}, {"provider", 3}, {"monitor", 4}};
constexpr Name kWeather[] = {{"bottom", 0}, {"top", 1}, {"off", 2}};
constexpr Name kSleepModes[] = {{"brief", 0}, {"reader", 1}};
constexpr Name kSleepSections[] = {{"reading", 1}, {"study", 2}, {"weather", 3}, {"today", 4}};

template <size_t N>
bool lookup(const Name (&names)[N], const char* text, uint8_t& value) {
  if (!text) return false;
  for (const auto& name : names)
    if (strcmp(name.text, text) == 0) {
      value = name.value;
      return true;
    }
  return false;
}

template <size_t N>
const char* nameOf(const Name (&names)[N], uint8_t value) {
  for (const auto& name : names)
    if (name.value == value) return name.text;
  return "";
}

// Every key must be one of `allowed`, and every allowed key must be present.
template <size_t N>
bool exactKeys(JsonObjectConst object, const char* const (&allowed)[N]) {
  size_t seen = 0;
  for (JsonPairConst pair : object) {
    bool known = false;
    for (const char* key : allowed) known = known || strcmp(pair.key().c_str(), key) == 0;
    if (!known) return false;
    ++seen;
  }
  return seen == N;
}

template <typename T, size_t N>
bool readIds(JsonVariantConst value, const Name (&names)[N], T* out, uint8_t cap, uint8_t& count) {
  if (!value.is<JsonArrayConst>()) return false;
  const auto array = value.as<JsonArrayConst>();
  if (array.size() == 0 || array.size() > cap) return false;
  count = 0;
  for (JsonVariantConst item : array) {
    uint8_t id;
    if (!item.is<const char*>() || !lookup(names, item.as<const char*>(), id)) return false;
    out[count++] = static_cast<T>(id);
  }
  return true;
}

}  // namespace

bool parseJson(const char* json, const size_t length, Profile& out, const char*& error) {
  error = "invalid profile";
  if (!json || length == 0 || length > 1024) {
    error = "profile must be 1..1024 bytes";
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, json, length) != DeserializationError::Ok || !doc.is<JsonObjectConst>()) {
    error = "profile is not a JSON object";
    return false;
  }
  static constexpr const char* rootKeys[] = {"schema", "home", "sleep"};
  static constexpr const char* homeKeys[] = {"items", "dailyWord", "weather", "nextEvent"};
  static constexpr const char* sleepKeys[] = {"mode", "sections"};
  const auto root = doc.as<JsonObjectConst>();
  if (!exactKeys(root, rootKeys) || !root["schema"].is<int>() || root["schema"].as<int>() != 1) {
    error = "schema 1 with exactly home and sleep is required";
    return false;
  }
  const auto homeValue = root["home"];
  const auto sleepValue = root["sleep"];
  if (!homeValue.is<JsonObjectConst>() || !sleepValue.is<JsonObjectConst>()) return false;
  const auto home = homeValue.as<JsonObjectConst>();
  const auto sleep = sleepValue.as<JsonObjectConst>();
  if (!exactKeys(home, homeKeys) || !exactKeys(sleep, sleepKeys)) {
    error = "unknown or missing home/sleep field";
    return false;
  }
  Profile parsed;
  uint8_t id;
  if (!readIds(home["items"], kHomeItems, parsed.homeItems, HOME_ITEM_CAP, parsed.homeCount) ||
      !home["dailyWord"].is<bool>() || !home["nextEvent"].is<bool>() || !home["weather"].is<const char*>() ||
      !lookup(kWeather, home["weather"].as<const char*>(), id)) {
    error = "invalid home settings";
    return false;
  }
  parsed.dailyWord = home["dailyWord"].as<bool>();
  parsed.nextEvent = home["nextEvent"].as<bool>();
  parsed.weather = static_cast<WeatherPanel>(id);
  if (!sleep["mode"].is<const char*>() || !lookup(kSleepModes, sleep["mode"].as<const char*>(), id) ||
      !readIds(sleep["sections"], kSleepSections, parsed.sleepSections, SLEEP_SECTION_CAP, parsed.sleepCount)) {
    error = "invalid sleep settings";
    return false;
  }
  parsed.sleepMode = static_cast<SleepMode>(id);
  if (!valid(parsed)) {
    error = "items and sections must be distinct and non-empty";
    return false;
  }
  out = parsed;
  return true;
}

size_t writeJson(const Profile& p, const uint32_t generation, const char* deviceId, char* out, const size_t cap) {
  if (!out || !cap || !valid(p)) return 0;
  JsonDocument doc;
  doc["schema"] = 1;
  doc["deviceID"] = deviceId ? deviceId : "";
  doc["generation"] = generation;
  auto home = doc["home"].to<JsonObject>();
  auto items = home["items"].to<JsonArray>();
  for (uint8_t i = 0; i < p.homeCount; ++i) items.add(nameOf(kHomeItems, static_cast<uint8_t>(p.homeItems[i])));
  home["dailyWord"] = p.dailyWord;
  home["weather"] = nameOf(kWeather, static_cast<uint8_t>(p.weather));
  home["nextEvent"] = p.nextEvent;
  auto sleep = doc["sleep"].to<JsonObject>();
  sleep["mode"] = nameOf(kSleepModes, static_cast<uint8_t>(p.sleepMode));
  auto sections = sleep["sections"].to<JsonArray>();
  for (uint8_t i = 0; i < p.sleepCount; ++i)
    sections.add(nameOf(kSleepSections, static_cast<uint8_t>(p.sleepSections[i])));
  auto caps = doc["capabilities"].to<JsonObject>();
  auto homeIds = caps["homeItems"].to<JsonArray>();
  for (const auto& name : kHomeItems) homeIds.add(name.text);
  caps["maxHomeItems"] = HOME_ITEM_CAP;
  auto weather = caps["weather"].to<JsonArray>();
  for (const auto& name : kWeather) weather.add(name.text);
  auto modes = caps["sleepModes"].to<JsonArray>();
  for (const auto& name : kSleepModes) modes.add(name.text);
  auto sleepIds = caps["sleepSections"].to<JsonArray>();
  for (const auto& name : kSleepSections) sleepIds.add(name.text);
  const size_t needed = measureJson(doc);
  if (needed + 1 > cap) return 0;
  return serializeJson(doc, out, cap);
}

}  // namespace PocketDaily::DailyProfile
