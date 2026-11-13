#include <ArduinoJson.h>

#include <cstring>

#include "PocketReadingShelf.h"
namespace PocketDaily::ReadingShelf {
bool parseJson(const char* json, const size_t size, Selection& s, bool& outOfMemory) {
  s = {};
  outOfMemory = false;
  if (!json || !size || size > 8192) return false;
  JsonDocument doc;
  const auto result = deserializeJson(doc, json, size);
  outOfMemory = result == DeserializationError::NoMemory;
  if (result || !doc.is<JsonObjectConst>()) return false;
  const auto o = doc.as<JsonObjectConst>();
  if (o.size() != 3 || !o["schema"].is<int>() || o["schema"].as<int>() != 1 || !o["automatic"].is<bool>() ||
      !o["paths"].is<JsonArrayConst>() || o["paths"].size() > CAP)
    return false;
  s.automatic = o["automatic"].as<bool>();
  for (const auto value : o["paths"].as<JsonArrayConst>()) {
    if (!value.is<const char*>()) {
      s = {};
      return false;
    }
    const auto path = value.as<JsonString>();
    if (!path.size() || path.size() >= PATH_BYTES || memchr(path.c_str(), 0, path.size())) {
      s = {};
      return false;
    }
    memcpy(s.paths[s.count++], path.c_str(), path.size());
  }
  if (valid(s)) return true;
  s = {};
  return false;
}
}  // namespace PocketDaily::ReadingShelf
