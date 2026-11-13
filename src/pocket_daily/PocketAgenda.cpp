#include "PocketAgenda.h"

#include <cstdio>
#include <cstring>

#include "ContentChecksum.h"
#include "TextValidation.h"
#include "home/GlanceFormat.h"
namespace PocketDaily::Agenda {
namespace {
bool text(const char* s, size_t n, bool required = false) {
  const size_t len = strnlen(s, n);
  if (len == n || (required && !len) || !Text::validUtf8(s, len, false)) return false;
  for (size_t i = len + 1; i < n; ++i)
    if (s[i]) return false;
  return true;
}
bool dateValid(uint32_t date) {
  char iso[11];
  if (date < 19700101 || date > 99991231) return false;
  snprintf(iso, sizeof(iso), "%04u-%02u-%02u", unsigned(date / 10000), unsigned(date / 100 % 100),
           unsigned(date % 100));
  const unsigned y = date / 10000, m = date / 100 % 100, d = date % 100;
  static constexpr unsigned lengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (!GlanceFormat::validIsoDate(iso) || m < 1 || m > 12) return false;
  const unsigned maxDay = lengths[m - 1] + (m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
  return d >= 1 && d <= maxDay;
}
uint32_t crc(const uint8_t* b, size_t n) { return ~Content::contentCrcUpdate(UINT32_MAX, b, n); }
void put(uint8_t* b, size_t& at, uint32_t v, size_t n) {
  for (size_t i = 0; i < n; ++i) b[at++] = v >> (8 * i);
}
uint32_t get(const uint8_t* b, size_t& at, size_t n) {
  uint32_t v = 0;
  for (size_t i = 0; i < n; ++i) v |= uint32_t(b[at++]) << (8 * i);
  return v;
}
}  // namespace
void Snapshot::clear() {
  dayCount = 0;
  for (auto& day : days) {
    day.date = 0;
    day.omittedEventCount = 0;
    day.eventCount = 0;
    for (auto& event : day.events) event.clear();
  }
}
uint32_t parseDate(const char* iso) {
  if (!iso || !GlanceFormat::validIsoDate(iso)) return 0;
  uint32_t result = 0;
  for (int i = 0; i < 10; ++i)
    if (i != 4 && i != 7) result = result * 10 + uint32_t(iso[i] - '0');
  return dateValid(result) ? result : 0;
}
uint32_t shiftDate(uint32_t date, int direction) {
  if (!dateValid(date)) return 0;
  if (!direction) return date;
  int y = date / 10000, m = date / 100 % 100, d = date % 100;
  if (direction > 0) {
    if (dateValid(date + 1)) return date + 1;
    d = 1;
    if (++m > 12) {
      m = 1;
      ++y;
    }
  } else {
    if (d > 1) return date - 1;
    if (--m < 1) {
      m = 12;
      --y;
    }
    d = 31;
    while (d > 27 && !dateValid(uint32_t(y * 10000 + m * 100 + d))) --d;
  }
  const uint32_t result = y * 10000 + m * 100 + d;
  return dateValid(result) ? result : date;
}
const Day* findDay(const Snapshot& s, uint32_t date) {
  for (uint8_t i = 0; i < s.dayCount && i < DAY_CAP; ++i)
    if (s.days[i].date == date) return &s.days[i];
  return nullptr;
}
bool valid(const Snapshot& s) {
  if (s.dayCount > DAY_CAP) return false;
  uint32_t previous = 0;
  for (uint8_t i = 0; i < DAY_CAP; ++i) {
    const auto& d = s.days[i];
    const bool used = i < s.dayCount;
    if (used) {
      if (!dateValid(d.date) || d.date <= previous || d.eventCount > Glance::EVENT_CAP) return false;
      previous = d.date;
    } else if (d.date || d.eventCount || d.omittedEventCount)
      return false;
    for (uint8_t j = 0; j < Glance::EVENT_CAP; ++j) {
      const auto& e = d.events[j];
      if (!text(e.startHm, sizeof(e.startHm)) || !text(e.endHm, sizeof(e.endHm)) ||
          !text(e.title, sizeof(e.title), j < d.eventCount))
        return false;
      if (j >= d.eventCount) {
        if (e.startHm[0] || e.endHm[0] || e.title[0]) return false;
        continue;
      }
      if (bool(e.startHm[0]) != bool(e.endHm[0])) return false;
      if (e.startHm[0] && (GlanceFormat::hmToMinutes(e.startHm) < 0 ||
                           GlanceFormat::hmToMinutes(e.endHm) < GlanceFormat::hmToMinutes(e.startHm)))
        return false;
    }
  }
  return true;
}
bool encodeRecord(const Snapshot& s, uint8_t (&b)[RECORD_BYTES]) {
  if (!valid(s)) return false;
  memset(b, 0, sizeof(b));
  memcpy(b, "PDAG", 4);
  size_t at = 4;
  put(b, at, 1, 1);
  put(b, at, 0, 1);
  put(b, at, RECORD_BYTES - 12, 2);
  put(b, at, s.dayCount, 1);
  for (const auto& d : s.days) {
    put(b, at, d.date, 4);
    put(b, at, d.omittedEventCount, 2);
    put(b, at, d.eventCount, 1);
    for (const auto& e : d.events) {
      memcpy(b + at, e.startHm, 6);
      at += 6;
      memcpy(b + at, e.endHm, 6);
      at += 6;
      memcpy(b + at, e.title, 49);
      at += 49;
    }
  }
  if (at != RECORD_BYTES - 4) return false;
  put(b, at, crc(b, at), 4);
  return true;
}
bool decodeRecord(const uint8_t* b, size_t n, Snapshot& s) {
  s.clear();
  if (!b || n != RECORD_BYTES || memcmp(b, "PDAG", 4) || b[4] != 1 || b[5]) return false;
  size_t at = 6;
  if (get(b, at, 2) != RECORD_BYTES - 12) return false;
  at = RECORD_BYTES - 4;
  if (get(b, at, 4) != crc(b, RECORD_BYTES - 4)) return false;
  at = 8;
  s.dayCount = get(b, at, 1);
  for (auto& d : s.days) {
    d.date = get(b, at, 4);
    d.omittedEventCount = get(b, at, 2);
    d.eventCount = get(b, at, 1);
    for (auto& e : d.events) {
      memcpy(e.startHm, b + at, 6);
      at += 6;
      memcpy(e.endHm, b + at, 6);
      at += 6;
      memcpy(e.title, b + at, 49);
      at += 49;
    }
  }
  if (!valid(s)) {
    s.clear();
    return false;
  }
  return true;
}
}  // namespace PocketDaily::Agenda
