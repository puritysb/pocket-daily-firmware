#include "PocketGlance.h"

#include <cstring>

#include "ContentChecksum.h"
#include "TextValidation.h"
#include "home/GlanceFormat.h"

namespace PocketDaily::AppGlance {
namespace {
constexpr uint8_t RECORD_VERSION = 1;
constexpr size_t HEADER_BYTES = 8;
constexpr size_t CRC_BYTES = 4;
constexpr size_t PAYLOAD_BYTES = RECORD_BYTES - HEADER_BYTES - CRC_BYTES;
constexpr size_t DAY_BYTES = 11 + 12 + 2 + 1 + 1 + 1;
constexpr size_t WEATHER_BYTES = 24 + 2 + 1 + 12 + 1 + 1 + 6 + 6 + 1 + 1 + WEATHER_DAY_CAP * DAY_BYTES;
constexpr size_t EVENT_BYTES = 6 + 6 + 49;
static_assert(PAYLOAD_BYTES == 4 + 2 + 6 + 1 + WEATHER_BYTES + 1 + Glance::EVENT_CAP * EVENT_BYTES,
              "glance record layout");

// NUL-terminated inside `capacity`, zero-filled after the terminator (one
// canonical encoding), strict UTF-8 without controls.
bool validString(const char* text, const size_t capacity, const bool required) {
  const size_t length = strnlen(text, capacity);
  if (length == capacity || (required && length == 0)) return false;
  for (size_t i = length + 1; i < capacity; ++i)
    if (text[i]) return false;
  return Text::validUtf8(text, length, false);
}

bool validHm(const char* hm, const size_t capacity, const bool required) {
  if (!validString(hm, capacity, false)) return false;
  if (!hm[0]) return !required;
  return GlanceFormat::hmToMinutes(hm) >= 0;
}

bool inRange(const int value, const int low, const int high) { return value >= low && value <= high; }
bool validTemp(const int value) { return inRange(value, -100, 100); }
bool validCode(const int value) { return inRange(value, -1, 99); }
bool validPercent(const int value) { return inRange(value, -1, 100); }

bool validDay(const DayWeather& day) {
  return validString(day.date, sizeof(day.date), true) && GlanceFormat::validIsoDate(day.date) &&
         validString(day.summary, sizeof(day.summary), false) && validCode(day.code) && validTemp(day.minC) &&
         validTemp(day.maxC) && validPercent(day.rainProbability);
}

bool validWeather(const Weather& w) {
  if (!validString(w.place, sizeof(w.place), false) || !validString(w.summary, sizeof(w.summary), false) ||
      !validCode(w.code) || !validTemp(w.tempC) || !validTemp(w.todayMinC) || !validTemp(w.todayMaxC) ||
      !validHm(w.rainStartHm, sizeof(w.rainStartHm), false) || !validHm(w.rainEndHm, sizeof(w.rainEndHm), false) ||
      !validPercent(w.rainProbability) || w.dayCount > WEATHER_DAY_CAP)
    return false;
  for (uint8_t i = 0; i < w.dayCount; ++i)
    if (!validDay(w.days[i])) return false;
  return true;
}

class Writer {
 public:
  explicit Writer(uint8_t* bytes) : bytes(bytes) {}
  void u8(const uint8_t value) { bytes[at++] = value; }
  void u16(const uint16_t value) {
    u8(static_cast<uint8_t>(value));
    u8(static_cast<uint8_t>(value >> 8));
  }
  void u32(const uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) u8(static_cast<uint8_t>(value >> (8 * i)));
  }
  void i8(const int8_t value) { u8(static_cast<uint8_t>(value)); }
  void i16(const int16_t value) { u16(static_cast<uint16_t>(value)); }
  void text(const char* value, const size_t capacity) {
    memcpy(bytes + at, value, capacity);
    at += capacity;
  }
  size_t size() const { return at; }

 private:
  uint8_t* bytes;
  size_t at = 0;
};

class Reader {
 public:
  explicit Reader(const uint8_t* bytes) : bytes(bytes) {}
  uint8_t u8() { return bytes[at++]; }
  uint16_t u16() {
    const uint16_t low = u8();
    return static_cast<uint16_t>(low | (static_cast<uint16_t>(u8()) << 8));
  }
  uint32_t u32() {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= static_cast<uint32_t>(u8()) << (8 * i);
    return value;
  }
  int8_t i8() { return static_cast<int8_t>(u8()); }
  int16_t i16() { return static_cast<int16_t>(u16()); }
  void text(char* value, const size_t capacity) {
    memcpy(value, bytes + at, capacity);
    at += capacity;
  }
  // Unused slots are stored as zeros so every snapshot has one encoding.
  bool zeros(const size_t count) {
    bool clear = true;
    for (size_t i = 0; i < count; ++i) clear = u8() == 0 && clear;
    return clear;
  }
  size_t size() const { return at; }

 private:
  const uint8_t* bytes;
  size_t at = 0;
};

uint32_t crc32(const uint8_t* bytes, const size_t size) { return ~Content::contentCrcUpdate(0xFFFFFFFFu, bytes, size); }
}  // namespace

void Snapshot::clear() {
  glance.clear();
  savedEpoch = 0;
  utcOffsetMinutes = 0;
  memset(syncedHm, 0, sizeof(syncedHm));
}

bool valid(const Snapshot& s) {
  const Glance& g = s.glance;
  if (s.savedEpoch < MIN_EPOCH || !inRange(s.utcOffsetMinutes, MIN_UTC_OFFSET_MINUTES, MAX_UTC_OFFSET_MINUTES) ||
      !validHm(s.syncedHm, sizeof(s.syncedHm), true))
    return false;
  if (g.usageCount != 0 || g.wrapupCount != 0 || g.eventCount > Glance::EVENT_CAP) return false;
  if (g.weather.valid && !validWeather(g.weather)) return false;
  for (uint8_t i = 0; i < g.eventCount; ++i) {
    const Event& e = g.events[i];
    if (!validHm(e.startHm, sizeof(e.startHm), false) || !validHm(e.endHm, sizeof(e.endHm), false) ||
        !validString(e.title, sizeof(e.title), true))
      return false;
  }
  return g.valid == (g.weather.valid || g.eventCount > 0);
}

bool encodeRecord(const Snapshot& s, uint8_t (&bytes)[RECORD_BYTES]) {
  if (!valid(s)) return false;
  memset(bytes, 0, sizeof(bytes));
  Writer out(bytes);
  out.text("PDGL", 4);
  out.u8(RECORD_VERSION);
  out.u8(0);
  out.u16(static_cast<uint16_t>(PAYLOAD_BYTES));
  out.u32(s.savedEpoch);
  out.i16(s.utcOffsetMinutes);
  out.text(s.syncedHm, sizeof(s.syncedHm));
  const Weather& w = s.glance.weather;
  out.u8(w.valid ? 1 : 0);
  // An absent weather block is stored as zeros, not as its sentinel values.
  if (w.valid) {
    out.text(w.place, sizeof(w.place));
    out.i16(w.code);
    out.i8(w.tempC);
    out.text(w.summary, sizeof(w.summary));
    out.i8(w.todayMinC);
    out.i8(w.todayMaxC);
    out.text(w.rainStartHm, sizeof(w.rainStartHm));
    out.text(w.rainEndHm, sizeof(w.rainEndHm));
    out.i8(w.rainProbability);
    out.u8(w.dayCount);
    for (uint8_t i = 0; i < WEATHER_DAY_CAP; ++i) {
      DayWeather day = w.days[i];
      if (i >= w.dayCount) memset(&day, 0, sizeof(day));
      out.text(day.date, sizeof(day.date));
      out.text(day.summary, sizeof(day.summary));
      out.i16(day.code);
      out.i8(day.minC);
      out.i8(day.maxC);
      out.i8(day.rainProbability);
    }
  } else {
    for (size_t i = 0; i < WEATHER_BYTES; ++i) out.u8(0);
  }
  out.u8(s.glance.eventCount);
  for (uint8_t i = 0; i < Glance::EVENT_CAP; ++i) {
    Event event = s.glance.events[i];
    if (i >= s.glance.eventCount) event.clear();
    out.text(event.startHm, sizeof(event.startHm));
    out.text(event.endHm, sizeof(event.endHm));
    out.text(event.title, sizeof(event.title));
  }
  if (out.size() != RECORD_BYTES - CRC_BYTES) return false;
  out.u32(crc32(bytes, RECORD_BYTES - CRC_BYTES));
  return true;
}

bool decodeRecord(const uint8_t* bytes, const size_t size, Snapshot& s) {
  s.clear();
  if (!bytes || size != RECORD_BYTES || memcmp(bytes, "PDGL", 4) != 0 || bytes[4] != RECORD_VERSION || bytes[5] != 0)
    return false;
  Reader in(bytes + 4 + 2);
  if (in.u16() != PAYLOAD_BYTES) return false;
  Reader crcIn(bytes + RECORD_BYTES - CRC_BYTES);
  if (crcIn.u32() != crc32(bytes, RECORD_BYTES - CRC_BYTES)) return false;

  Reader payload(bytes + HEADER_BYTES);
  s.savedEpoch = payload.u32();
  s.utcOffsetMinutes = payload.i16();
  payload.text(s.syncedHm, sizeof(s.syncedHm));
  const uint8_t hasWeather = payload.u8();
  Weather& w = s.glance.weather;
  if (hasWeather > 1) {
    s.clear();
    return false;
  }
  if (hasWeather) {
    w.valid = true;
    payload.text(w.place, sizeof(w.place));
    w.code = payload.i16();
    w.tempC = payload.i8();
    payload.text(w.summary, sizeof(w.summary));
    w.todayMinC = payload.i8();
    w.todayMaxC = payload.i8();
    payload.text(w.rainStartHm, sizeof(w.rainStartHm));
    payload.text(w.rainEndHm, sizeof(w.rainEndHm));
    w.rainProbability = payload.i8();
    w.dayCount = payload.u8();
    if (w.dayCount > WEATHER_DAY_CAP) {
      s.clear();
      return false;
    }
    for (uint8_t i = 0; i < WEATHER_DAY_CAP; ++i) {
      DayWeather& day = w.days[i];
      if (i >= w.dayCount) {
        if (!payload.zeros(DAY_BYTES)) {
          s.clear();
          return false;
        }
        continue;
      }
      payload.text(day.date, sizeof(day.date));
      payload.text(day.summary, sizeof(day.summary));
      day.code = payload.i16();
      day.minC = payload.i8();
      day.maxC = payload.i8();
      day.rainProbability = payload.i8();
    }
    if (w.dayCount >= 2) w.tomorrow = w.days[1];
  } else if (!payload.zeros(WEATHER_BYTES)) {
    s.clear();
    return false;
  }
  s.glance.eventCount = payload.u8();
  if (s.glance.eventCount > Glance::EVENT_CAP) {
    s.clear();
    return false;
  }
  for (uint8_t i = 0; i < Glance::EVENT_CAP; ++i) {
    Event& event = s.glance.events[i];
    if (i >= s.glance.eventCount) {
      if (!payload.zeros(EVENT_BYTES)) {
        s.clear();
        return false;
      }
      continue;
    }
    payload.text(event.startHm, sizeof(event.startHm));
    payload.text(event.endHm, sizeof(event.endHm));
    payload.text(event.title, sizeof(event.title));
  }
  s.glance.valid = w.valid || s.glance.eventCount > 0;
  if (payload.size() != PAYLOAD_BYTES || !valid(s)) {
    s.clear();
    return false;
  }
  return true;
}
}  // namespace PocketDaily::AppGlance
