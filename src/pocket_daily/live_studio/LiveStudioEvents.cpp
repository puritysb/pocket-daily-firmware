#include "LiveStudioEvents.h"

#include <cstdio>
#include <cstring>

namespace PocketDaily::LiveStudio {

bool encodeHello(char* out, size_t cap, const char* deviceId, const char* version) {
  if (!out || cap == 0) return false;
  const int written = std::snprintf(
      out, cap, "{\"hello\":{\"proto\":\"%s\",\"deviceID\":\"%s\",\"version\":\"%s\",\"caps\":[\"status\",\"prefs\"]}}",
      kProtocol, deviceId ? deviceId : "", version ? version : "");
  return written > 0 && static_cast<size_t>(written) < cap;
}

bool encodePong(char* out, size_t cap) {
  if (!out || cap == 0) return false;
  const int written = std::snprintf(out, cap, "{\"pong\":{}}");
  return written > 0 && static_cast<size_t>(written) < cap;
}

bool encodeBye(char* out, size_t cap) {
  if (!out || cap == 0) return false;
  const int written = std::snprintf(out, cap, "{\"bye\":{}}");
  return written > 0 && static_cast<size_t>(written) < cap;
}

bool encodePrefsChanged(char* out, size_t cap) {
  if (!out || cap == 0) return false;
  const int written = std::snprintf(out, cap, "{\"prefs\":{\"changed\":true}}");
  return written > 0 && static_cast<size_t>(written) < cap;
}

bool encodeStatusEvent(char* out, size_t cap, const char* statusJson) {
  if (!out || cap == 0 || !statusJson) return false;
  const size_t body = std::strlen(statusJson);
  // Total written: "{\"status\":" (10) + body + '}' + NUL.
  if (body + 12 > cap) return false;
  std::memcpy(out, "{\"status\":", 10);
  std::memcpy(out + 10, statusJson, body);
  out[10 + body] = '}';
  out[11 + body] = '\0';
  return true;
}

namespace {

// Bounded search for `"key"` inside [begin, end). Returns nullptr when absent.
const char* findKey(const char* begin, const char* end, const char* key) {
  const size_t keyLen = std::strlen(key);
  for (const char* p = begin; p + keyLen <= end; p++) {
    if (std::memcmp(p, key, keyLen) == 0) return p;
  }
  return nullptr;
}

}  // namespace

ClientMessage parseClientMessage(const char* payload, size_t length) {
  if (!payload || length == 0 || payload[0] != '{') return ClientMessage::None;
  const char* begin = payload;
  const char* end = payload + length;

  if (findKey(begin, end, "\"ping\"") != nullptr) return ClientMessage::Ping;
  if (findKey(begin, end, "\"unsubscribe\"") != nullptr) return ClientMessage::Unsubscribe;
  if (findKey(begin, end, "\"subscribe\"") != nullptr) return ClientMessage::Subscribe;
  return ClientMessage::None;
}

}  // namespace PocketDaily::LiveStudio
