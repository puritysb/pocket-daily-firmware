#include "PocketProfile.h"

#include <ArduinoJson.h>

#include <cstring>

#include "ContentChecksum.h"

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

template <typename T>
bool distinctKnown(const T* values, uint8_t count, uint8_t cap, uint8_t maxValue) {
  if (count == 0 || count > cap) return false;
  for (uint8_t i = 0; i < count; ++i) {
    const auto v = static_cast<uint8_t>(values[i]);
    if (v < 1 || v > maxValue) return false;
    for (uint8_t j = 0; j < i; ++j)
      if (values[j] == values[i]) return false;
  }
  return true;
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

uint32_t u32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
void put32(uint8_t* p, uint32_t n) {
  for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(n >> (8 * i));
}
uint32_t crc32(const uint8_t* bytes, size_t size) { return ~Content::contentCrcUpdate(0xFFFFFFFFu, bytes, size); }
}  // namespace

bool Profile::shows(const HomeItem item) const {
  for (uint8_t i = 0; i < homeCount; ++i)
    if (homeItems[i] == item) return true;
  return false;
}

bool Profile::sleeps(const SleepSection section) const {
  for (uint8_t i = 0; i < sleepCount; ++i)
    if (sleepSections[i] == section) return true;
  return false;
}

Profile defaults() {
  Profile p;
  p.homeItems[0] = HomeItem::Reading;
  p.homeItems[1] = HomeItem::Study;
  p.homeItems[2] = HomeItem::Provider;
  p.homeCount = 3;
  p.sleepSections[0] = SleepSection::Reading;
  p.sleepSections[1] = SleepSection::Study;
  p.sleepSections[2] = SleepSection::Weather;
  p.sleepSections[3] = SleepSection::Today;
  p.sleepCount = 4;
  return p;
}

bool operator==(const Profile& a, const Profile& b) {
  if (a.homeCount != b.homeCount || a.sleepCount != b.sleepCount || a.dailyWord != b.dailyWord ||
      a.weather != b.weather || a.nextEvent != b.nextEvent || a.sleepMode != b.sleepMode)
    return false;
  for (uint8_t i = 0; i < a.homeCount && i < HOME_ITEM_CAP; ++i)
    if (a.homeItems[i] != b.homeItems[i]) return false;
  for (uint8_t i = 0; i < a.sleepCount && i < SLEEP_SECTION_CAP; ++i)
    if (a.sleepSections[i] != b.sleepSections[i]) return false;
  return true;
}

bool valid(const Profile& p) {
  return distinctKnown(p.homeItems, p.homeCount, HOME_ITEM_CAP, 4) &&
         distinctKnown(p.sleepSections, p.sleepCount, SLEEP_SECTION_CAP, 4) && static_cast<uint8_t>(p.weather) <= 2 &&
         static_cast<uint8_t>(p.sleepMode) <= 1;
}

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

bool encodeRecord(const Profile& p, const uint32_t generation, uint8_t (&bytes)[RECORD_BYTES]) {
  if (generation == 0 || !valid(p)) return false;
  memset(bytes, 0, RECORD_BYTES);
  memcpy(bytes, "PDP1", 4);
  put32(bytes + 4, generation);
  bytes[8] = 1;  // schema
  bytes[9] = p.homeCount;
  for (uint8_t i = 0; i < p.homeCount; ++i) bytes[10 + i] = static_cast<uint8_t>(p.homeItems[i]);
  bytes[14] = static_cast<uint8_t>((p.dailyWord ? 1 : 0) | (p.nextEvent ? 2 : 0));
  bytes[15] = static_cast<uint8_t>(p.weather);
  bytes[16] = static_cast<uint8_t>(p.sleepMode);
  bytes[17] = p.sleepCount;
  for (uint8_t i = 0; i < p.sleepCount; ++i) bytes[18 + i] = static_cast<uint8_t>(p.sleepSections[i]);
  put32(bytes + 28, crc32(bytes, 28));
  return true;
}

bool decodeRecord(const uint8_t* bytes, const size_t size, Profile& profile, uint32_t& generation) {
  if (!bytes || size != RECORD_BYTES || memcmp(bytes, "PDP1", 4) != 0 || bytes[8] != 1 ||
      u32(bytes + 28) != crc32(bytes, 28))
    return false;
  Profile p;
  p.homeCount = bytes[9];
  p.sleepCount = bytes[17];
  if (p.homeCount > HOME_ITEM_CAP || p.sleepCount > SLEEP_SECTION_CAP || (bytes[14] & ~3u) != 0) return false;
  for (uint8_t i = 0; i < p.homeCount; ++i) p.homeItems[i] = static_cast<HomeItem>(bytes[10 + i]);
  p.dailyWord = (bytes[14] & 1) != 0;
  p.nextEvent = (bytes[14] & 2) != 0;
  p.weather = static_cast<WeatherPanel>(bytes[15]);
  p.sleepMode = static_cast<SleepMode>(bytes[16]);
  for (uint8_t i = 0; i < p.sleepCount; ++i) p.sleepSections[i] = static_cast<SleepSection>(bytes[18 + i]);
  // Canonical form: unused bytes stay zero, so a record has one encoding.
  uint8_t canonical[RECORD_BYTES];
  const uint32_t gen = u32(bytes + 4);
  if (!valid(p) || !encodeRecord(p, gen, canonical) || memcmp(canonical, bytes, RECORD_BYTES) != 0) return false;
  profile = p;
  generation = gen;
  return true;
}
}  // namespace PocketDaily::DailyProfile
