#include <ArduinoJson.h>

#include <cstring>

#include "PocketGlance.h"

// JSON codec for POST /api/pocket/v1/glance (device and host tests). The
// renderer and the record store need only PocketGlance.cpp, not ArduinoJson.
namespace PocketDaily::AppGlance {
namespace {
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

// Copies a JSON string of at most capacity - 1 bytes. Content rules (UTF-8,
// controls, formats) are checked afterwards by valid() on the whole snapshot;
// an embedded NUL makes the stored text shorter than the JSON length and is
// rejected here.
bool copyString(JsonVariantConst value, char* out, const size_t capacity) {
  if (!value.is<const char*>()) return false;
  const JsonString text = value.as<JsonString>();
  if (text.size() >= capacity || memchr(text.c_str(), '\0', text.size())) return false;
  memcpy(out, text.c_str(), text.size());
  out[text.size()] = '\0';
  return true;
}

bool readInt(JsonVariantConst value, const int low, const int high, int& out) {
  if (!value.is<int>()) return false;
  out = value.as<int>();
  return out >= low && out <= high;
}

template <typename T>
bool readNarrow(JsonVariantConst value, const int low, const int high, T& out) {
  int parsed = 0;
  if (!readInt(value, low, high, parsed)) return false;
  out = static_cast<T>(parsed);
  return true;
}

bool readDay(JsonVariantConst value, DayWeather& day) {
  static constexpr const char* keys[] = {"date", "summary", "code", "minC", "maxC", "rainProbability"};
  if (!value.is<JsonObjectConst>()) return false;
  const auto object = value.as<JsonObjectConst>();
  return exactKeys(object, keys) && copyString(object["date"], day.date, sizeof(day.date)) &&
         copyString(object["summary"], day.summary, sizeof(day.summary)) &&
         readNarrow(object["code"], -1, 99, day.code) && readNarrow(object["minC"], -100, 100, day.minC) &&
         readNarrow(object["maxC"], -100, 100, day.maxC) &&
         readNarrow(object["rainProbability"], -1, 100, day.rainProbability);
}

bool readWeather(JsonObjectConst object, Weather& w, const char*& error) {
  static constexpr const char* keys[] = {"place",     "code",        "tempC",     "summary",         "todayMinC",
                                         "todayMaxC", "rainStartHm", "rainEndHm", "rainProbability", "days"};
  if (!exactKeys(object, keys)) {
    error = "unknown or missing weather field";
    return false;
  }
  w.valid = true;
  if (!copyString(object["place"], w.place, sizeof(w.place)) ||
      !copyString(object["summary"], w.summary, sizeof(w.summary)) ||
      !copyString(object["rainStartHm"], w.rainStartHm, sizeof(w.rainStartHm)) ||
      !copyString(object["rainEndHm"], w.rainEndHm, sizeof(w.rainEndHm))) {
    error = "weather text is not a string or is too long";
    return false;
  }
  if (!readNarrow(object["code"], -1, 99, w.code) || !readNarrow(object["tempC"], -100, 100, w.tempC) ||
      !readNarrow(object["todayMinC"], -100, 100, w.todayMinC) ||
      !readNarrow(object["todayMaxC"], -100, 100, w.todayMaxC) ||
      !readNarrow(object["rainProbability"], -1, 100, w.rainProbability)) {
    error = "weather number is not an integer or is out of range";
    return false;
  }
  const JsonVariantConst days = object["days"];
  if (!days.is<JsonArrayConst>() || days.size() > WEATHER_DAY_CAP) {
    error = "weather days must be an array of at most 5";
    return false;
  }
  for (JsonVariantConst day : days.as<JsonArrayConst>()) {
    if (!readDay(day, w.days[w.dayCount])) {
      error = "invalid weather day";
      return false;
    }
    ++w.dayCount;
  }
  if (w.dayCount >= 2) w.tomorrow = w.days[1];
  return true;
}

bool readEvent(JsonVariantConst value, Event& event) {
  static constexpr const char* keys[] = {"startHm", "endHm", "title"};
  if (!value.is<JsonObjectConst>()) return false;
  const auto object = value.as<JsonObjectConst>();
  return exactKeys(object, keys) && copyString(object["startHm"], event.startHm, sizeof(event.startHm)) &&
         copyString(object["endHm"], event.endHm, sizeof(event.endHm)) &&
         copyString(object["title"], event.title, sizeof(event.title));
}
}  // namespace

bool parseJson(const char* json, const size_t length, Snapshot& out, const char*& error, bool& outOfMemory) {
  error = "invalid glance";
  outOfMemory = false;
  out.clear();
  if (!json || length == 0 || length > MAX_BODY_BYTES) {
    error = "glance must be 1..2048 bytes";
    return false;
  }
  JsonDocument doc;
  const DeserializationError parsed = deserializeJson(doc, json, length);
  if (parsed == DeserializationError::NoMemory) {
    outOfMemory = true;
    error = "reader memory is too low for the glance";
    return false;
  }
  if (parsed != DeserializationError::Ok || !doc.is<JsonObjectConst>()) {
    error = "glance is not a JSON object";
    return false;
  }
  static constexpr const char* rootKeys[] = {"schema",           "savedEpoch", "syncedHm",
                                             "utcOffsetMinutes", "weather",    "events"};
  const auto root = doc.as<JsonObjectConst>();
  if (!exactKeys(root, rootKeys)) {
    error = "unknown or missing glance field";
    return false;
  }
  if (!root["schema"].is<int>() || root["schema"].as<int>() != 1) {
    error = "schema 1 is required";
    return false;
  }
  // Assemble in place and clear on any failure so nothing is partially applied.
  Snapshot& s = out;
  const JsonVariantConst epoch = root["savedEpoch"];
  if (!epoch.is<uint32_t>() || epoch.as<uint32_t>() < MIN_EPOCH) {
    s.clear();
    error = "savedEpoch must be a unix time after 2023-11-14";
    return false;
  }
  s.savedEpoch = epoch.as<uint32_t>();
  if (!readNarrow(root["utcOffsetMinutes"], MIN_UTC_OFFSET_MINUTES, MAX_UTC_OFFSET_MINUTES, s.utcOffsetMinutes)) {
    s.clear();
    error = "utcOffsetMinutes must be an integer in -720..840";
    return false;
  }
  if (!copyString(root["syncedHm"], s.syncedHm, sizeof(s.syncedHm))) {
    s.clear();
    error = "syncedHm must be HH:MM";
    return false;
  }
  const JsonVariantConst weather = root["weather"];
  if (!weather.isNull()) {
    if (!weather.is<JsonObjectConst>()) {
      s.clear();
      error = "weather must be an object or null";
      return false;
    }
    if (!readWeather(weather.as<JsonObjectConst>(), s.glance.weather, error)) {
      s.clear();
      return false;
    }
  }
  const JsonVariantConst events = root["events"];
  if (!events.is<JsonArrayConst>() || events.size() > Glance::EVENT_CAP) {
    s.clear();
    error = "events must be an array of at most 3";
    return false;
  }
  for (JsonVariantConst event : events.as<JsonArrayConst>()) {
    if (!readEvent(event, s.glance.events[s.glance.eventCount])) {
      s.clear();
      error = "invalid event";
      return false;
    }
    ++s.glance.eventCount;
  }
  s.glance.valid = s.glance.weather.valid || s.glance.eventCount > 0;
  if (!valid(s)) {
    s.clear();
    error = "glance text, time or date is invalid";
    return false;
  }
  return true;
}
}  // namespace PocketDaily::AppGlance
