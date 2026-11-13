#include <ArduinoJson.h>

#include <cstdio>
#include <cstring>

#include "PocketAgenda.h"
namespace PocketDaily::Agenda {
namespace {
template <size_t N>
bool keys(JsonObjectConst o, const char* const (&names)[N]) {
  if (o.size() != N) return false;
  for (JsonPairConst pair : o) {
    bool known = false;
    for (const char* name : names) known |= strcmp(name, pair.key().c_str()) == 0;
    if (!known) return false;
  }
  return true;
}
bool copy(JsonVariantConst v, char* out, size_t cap) {
  if (!v.is<const char*>()) return false;
  const auto s = v.as<JsonString>();
  if (s.size() >= cap || memchr(s.c_str(), 0, s.size())) return false;
  memcpy(out, s.c_str(), s.size());
  out[s.size()] = 0;
  return true;
}
}  // namespace
bool parseJson(const char* bytes, size_t n, Snapshot& out, const char*& error, bool& oom) {
  out.clear();
  oom = false;
  error = "Invalid agenda document";
  if (!bytes || !n || n > MAX_BODY_BYTES) return false;
  JsonDocument doc;
  auto result = deserializeJson(doc, bytes, n);
  if (result) {
    oom = result == DeserializationError::NoMemory;
    return false;
  }
  static constexpr const char* rootKeys[] = {"schema", "days"};
  static constexpr const char* dayKeys[] = {"date", "events", "omittedEventCount"};
  static constexpr const char* eventKeys[] = {"startHm", "endHm", "title"};
  const auto root = doc.as<JsonObjectConst>();
  if (!keys(root, rootKeys) || !root["schema"].is<int>() || root["schema"].as<int>() != 1 ||
      !root["days"].is<JsonArrayConst>() || root["days"].size() > DAY_CAP)
    return false;
  for (JsonVariantConst day : root["days"].as<JsonArrayConst>()) {
    if (!day.is<JsonObjectConst>() || !keys(day.as<JsonObjectConst>(), dayKeys) || !day["date"].is<const char*>() ||
        !day["omittedEventCount"].is<unsigned int>() || day["omittedEventCount"].as<unsigned int>() > 65535 ||
        !day["events"].is<JsonArrayConst>() || day["events"].size() > Glance::EVENT_CAP) {
      out.clear();
      return false;
    }
    auto& d = out.days[out.dayCount++];
    char iso[11]{};
    if (!copy(day["date"], iso, sizeof(iso))) {
      out.clear();
      return false;
    }
    d.date = parseDate(iso);
    d.omittedEventCount = day["omittedEventCount"].as<uint16_t>();
    for (JsonVariantConst event : day["events"].as<JsonArrayConst>()) {
      auto& e = d.events[d.eventCount++];
      if (!event.is<JsonObjectConst>() || !keys(event.as<JsonObjectConst>(), eventKeys) ||
          !copy(event["startHm"], e.startHm, sizeof(e.startHm)) || !copy(event["endHm"], e.endHm, sizeof(e.endHm)) ||
          !copy(event["title"], e.title, sizeof(e.title))) {
        out.clear();
        return false;
      }
    }
  }
  if (!valid(out)) {
    out.clear();
    return false;
  }
  error = nullptr;
  return true;
}
bool writeJson(const Snapshot& s, char* bytes, size_t cap, size_t& size) {
  size = 0;
  if (!bytes || !valid(s)) return false;
  JsonDocument doc;
  doc["schema"] = 1;
  auto days = doc["days"].to<JsonArray>();
  for (uint8_t i = 0; i < s.dayCount; ++i) {
    const auto& d = s.days[i];
    auto day = days.add<JsonObject>();
    char iso[11];
    snprintf(iso, sizeof(iso), "%04u-%02u-%02u", unsigned(d.date / 10000), unsigned(d.date / 100 % 100),
             unsigned(d.date % 100));
    day["date"] = iso;
    day["omittedEventCount"] = d.omittedEventCount;
    auto events = day["events"].to<JsonArray>();
    for (uint8_t j = 0; j < d.eventCount; ++j) {
      const auto& e = d.events[j];
      auto event = events.add<JsonObject>();
      event["startHm"] = e.startHm;
      event["endHm"] = e.endHm;
      event["title"] = e.title;
    }
  }
  if (doc.overflowed() || measureJson(doc) >= cap) return false;
  size = serializeJson(doc, bytes, cap);
  return size > 0;
}
}  // namespace PocketDaily::Agenda
