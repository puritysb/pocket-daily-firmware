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

uint32_t parseNumberAfter(const char* p, const char* end, bool* ok) {
  while (p < end && (*p == ' ' || *p == ':')) p++;
  uint32_t value = 0;
  bool any = false;
  while (p < end && *p >= '0' && *p <= '9') {
    // Saturate instead of overflowing on absurd input.
    if (value < 429496729u) value = value * 10 + static_cast<uint32_t>(*p - '0');
    any = true;
    p++;
  }
  *ok = any;
  return value;
}

uint32_t clampInterval(uint32_t requested) {
  if (requested < kMinCaptureIntervalMs) return kMinCaptureIntervalMs;
  if (requested > 60000) return 60000;
  return requested;
}

}  // namespace

ClientMessage parseClientMessage(const char* payload, size_t length, Subscription* out) {
  if (!payload || length == 0 || payload[0] != '{') return ClientMessage::None;
  const char* begin = payload;
  const char* end = payload + length;

  if (findKey(begin, end, "\"ping\"") != nullptr) return ClientMessage::Ping;
  if (findKey(begin, end, "\"unsubscribe\"") != nullptr) return ClientMessage::Unsubscribe;

  const char* subscribe = findKey(begin, end, "\"subscribe\"");
  if (subscribe == nullptr) return ClientMessage::None;
  if (out == nullptr) return ClientMessage::Subscribe;

  Subscription parsed;
  // Scan only within a bounded window after the key; the messages are tiny.
  const char* scanEnd = (end - subscribe > 128) ? subscribe + 128 : end;
  const char* frames = findKey(subscribe, scanEnd, "\"frames\":true");
  if (frames != nullptr) {
    parsed.frames = true;
  }
  const char* interval = findKey(subscribe, scanEnd, "\"minIntervalMs\":");
  if (interval != nullptr) {
    bool ok = false;
    const uint32_t requested = parseNumberAfter(interval + 15, scanEnd, &ok);
    if (ok) parsed.minIntervalMs = clampInterval(requested);
  }
  *out = parsed;
  return ClientMessage::Subscribe;
}

bool shouldCaptureFrame(uint32_t freeHeap, uint32_t nowMs, uint32_t lastCaptureMs) {
  if (freeHeap < kMinCaptureFreeHeap) return false;
  return static_cast<uint32_t>(nowMs - lastCaptureMs) >= kMinCaptureIntervalMs;
}

}  // namespace PocketDaily::LiveStudio
