#pragma once
//
// GlanceFormat.h — pure formatting helpers for the Pocket Daily weather and
// schedule glance (Home, Daily Brief). Header-only and Arduino-free so the
// host ctest suite can pin the strings (test/pocket_glance). Wall-clock
// "HH:MM" values and ISO dates are the companion app's local time
// (docs/pocket-glance-v1.md); the reader keeps no timezone database and only
// applies the app's UTC offset where it needs today's local date.
//
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "pocket_daily/models.h"

namespace PocketDaily::GlanceFormat {

// "HH:MM" → minutes of day, -1 on malformed input.
inline int hmToMinutes(const char* hm) {
  if (!hm || strlen(hm) != 5 || hm[2] != ':') return -1;
  const int h = (hm[0] - '0') * 10 + (hm[1] - '0');
  const int m = (hm[3] - '0') * 10 + (hm[4] - '0');
  if (hm[0] < '0' || hm[0] > '9' || hm[1] < '0' || hm[1] > '9' || hm[3] < '0' || hm[3] > '9' || hm[4] < '0' ||
      hm[4] > '9' || h > 23 || m > 59)
    return -1;
  return h * 60 + m;
}

inline void minutesToHm(char* out, size_t cap, int minutesOfDay) {
  minutesOfDay %= 24 * 60;
  if (minutesOfDay < 0) minutesOfDay += 24 * 60;
  snprintf(out, cap, "%02d:%02d", minutesOfDay / 60, minutesOfDay % 60);
}

// baseHm advanced by addSec (day-wrapping). False on malformed baseHm.
inline bool addToHm(char* out, size_t cap, const char* baseHm, uint32_t addSec) {
  const int base = hmToMinutes(baseHm);
  if (base < 0) return false;
  minutesToHm(out, cap, base + (int)(addSec / 60));
  return true;
}

// ISO local date → compact weekday label. The app owns the timezone and sends
// the already-local date, so the reader needs no timezone database.
inline bool formatWeekday(char* out, size_t cap, const char* isoDate) {
  if (!out || cap == 0) return false;
  out[0] = '\0';
  if (!isoDate || strlen(isoDate) != 10 || isoDate[4] != '-' || isoDate[7] != '-') return false;
  static constexpr int digitPositions[] = {0, 1, 2, 3, 5, 6, 8, 9};
  for (int i : digitPositions)
    if (isoDate[i] < '0' || isoDate[i] > '9') return false;
  const int year = (isoDate[0] - '0') * 1000 + (isoDate[1] - '0') * 100 + (isoDate[2] - '0') * 10 + isoDate[3] - '0';
  const int month = (isoDate[5] - '0') * 10 + isoDate[6] - '0';
  const int day = (isoDate[8] - '0') * 10 + isoDate[9] - '0';
  if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31) return false;
  // Sakamoto: 0=Sunday. Calendar validity beyond the simple bounds is the
  // provider's responsibility; malformed transport still fails closed above.
  static constexpr int offset[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  static constexpr const char* names[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  int y = year;
  if (month < 3) y--;
  const int weekday = (y + y / 4 - y / 100 + y / 400 + offset[month - 1] + day) % 7;
  snprintf(out, cap, "%s", names[weekday]);
  return true;
}

// "28° Rain · 22–30°" (parts drop out when unknown). Returns chars written.
inline int formatWeatherNow(char* out, size_t cap, const Weather& w) {
  int o = 0;
  out[0] = '\0';
  if (w.tempC != GLANCE_TEMP_NONE) o += snprintf(out + o, cap - o, "%d\xC2\xB0", (int)w.tempC);
  if (w.summary[0]) o += snprintf(out + o, cap - o, "%s%s", o ? " " : "", w.summary);
  if (w.todayMinC != GLANCE_TEMP_NONE && w.todayMaxC != GLANCE_TEMP_NONE) {
    o += snprintf(out + o, cap - o, "%s%d\xE2\x80\x93%d\xC2\xB0", o ? " \xC2\xB7 " : "", (int)w.todayMinC,
                  (int)w.todayMaxC);
  }
  return o;
}

// "Rain ~15:00–17:00 70%" / "Rain ~15:00 70%". Empty when no window.
inline int formatRainLine(char* out, size_t cap, const Weather& w) {
  out[0] = '\0';
  if (!w.rainStartHm[0]) return 0;
  int o = snprintf(out, cap, "Rain ~%s", w.rainStartHm);
  if (w.rainEndHm[0]) o += snprintf(out + o, cap - o, "\xE2\x80\x93%s", w.rainEndHm);
  if (w.rainProbability >= 0) o += snprintf(out + o, cap - o, " %d%%", (int)w.rainProbability);
  return o;
}

// "09:30–10:00 Standup" / "09:30 Standup" / "Standup" (all-day, no times).
// Returns chars written; empty when the event has no title.
inline int formatEventLine(char* out, size_t cap, const Event& e) {
  out[0] = '\0';
  if (!e.title[0]) return 0;
  int o = 0;
  if (e.startHm[0]) {
    o += snprintf(out + o, cap - o, "%s", e.startHm);
    if (e.endHm[0]) o += snprintf(out + o, cap - o, "\xE2\x80\x93%s", e.endHm);
    o += snprintf(out + o, cap - o, " ");
  }
  o += snprintf(out + o, cap - o, "%s", e.title);
  return o;
}

// "Tomorrow Clear 22–31° · rain 10%". Empty when nothing is known.
inline int formatTomorrowLine(char* out, size_t cap, const DayWeather& t) {
  out[0] = '\0';
  if (!t.summary[0] && t.minC == GLANCE_TEMP_NONE) return 0;
  int o = snprintf(out, cap, "Tomorrow");
  if (t.summary[0]) o += snprintf(out + o, cap - o, " %s", t.summary);
  if (t.minC != GLANCE_TEMP_NONE && t.maxC != GLANCE_TEMP_NONE)
    o += snprintf(out + o, cap - o, " %d\xE2\x80\x93%d\xC2\xB0", (int)t.minC, (int)t.maxC);
  if (t.rainProbability > 0) o += snprintf(out + o, cap - o, " \xC2\xB7 rain %d%%", (int)t.rainProbability);
  return o;
}

// Strict "YYYY-MM-DD" with plausible month/day bounds (calendar validity beyond
// that is the sender's responsibility).
inline bool validIsoDate(const char* isoDate) {
  if (!isoDate || strlen(isoDate) != 10 || isoDate[4] != '-' || isoDate[7] != '-') return false;
  static constexpr int digitPositions[] = {0, 1, 2, 3, 5, 6, 8, 9};
  for (int i : digitPositions)
    if (isoDate[i] < '0' || isoDate[i] > '9') return false;
  const int year = (isoDate[0] - '0') * 1000 + (isoDate[1] - '0') * 100 + (isoDate[2] - '0') * 10 + isoDate[3] - '0';
  const int month = (isoDate[5] - '0') * 10 + isoDate[6] - '0';
  const int day = (isoDate[8] - '0') * 10 + isoDate[9] - '0';
  return year >= 1970 && month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

// Local calendar date of a unix time shifted by the app's UTC offset, as
// "YYYY-MM-DD" (civil-from-days; no timezone database). False before 1970.
inline bool formatLocalIsoDate(char* out, size_t cap, int64_t epochSec, int utcOffsetMinutes) {
  if (!out || cap < 11) return false;
  out[0] = '\0';
  const int64_t local = epochSec + static_cast<int64_t>(utcOffsetMinutes) * 60;
  if (local < 0) return false;
  int64_t z = local / 86400 + 719468;
  const int64_t era = z / 146097;
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  const int day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  const int month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  const int year = static_cast<int>(yoe + era * 400 + (month <= 2 ? 1 : 0));
  if (year > 9999) return false;
  snprintf(out, cap, "%04d-%02d-%02d", year, month, day);
  return true;
}

// Moves a glance saved on an earlier local day onto `todayIso`: today's
// schedule no longer applies, forecast days before today are dropped, and the
// "now" weather (temperature, rain window) is replaced by today's forecast
// day. A glance with no day left for today loses its weather. Idempotent; a
// glance saved today (or a clock behind the save) is left unchanged.
inline void rollToLocalDay(Glance& glance, const char* savedIso, const char* todayIso) {
  if (!validIsoDate(savedIso) || !validIsoDate(todayIso) || strcmp(savedIso, todayIso) >= 0) return;
  for (auto& event : glance.events) event.clear();
  glance.eventCount = 0;
  Weather& w = glance.weather;
  if (w.valid) {
    uint8_t first = 0;
    const uint8_t count = w.dayCount > WEATHER_DAY_CAP ? WEATHER_DAY_CAP : w.dayCount;
    while (first < count && strcmp(w.days[first].date, todayIso) < 0) ++first;
    if (first >= count) {
      w.clear();
    } else {
      for (uint8_t i = 0; i < WEATHER_DAY_CAP; ++i) {
        if (i + first < count)
          w.days[i] = w.days[i + first];
        else
          w.days[i].clear();
      }
      w.dayCount = static_cast<uint8_t>(count - first);
      const DayWeather& today = w.days[0];
      w.code = today.code;
      snprintf(w.summary, sizeof(w.summary), "%s", today.summary);
      w.tempC = GLANCE_TEMP_NONE;
      w.todayMinC = today.minC;
      w.todayMaxC = today.maxC;
      w.rainStartHm[0] = '\0';
      w.rainEndHm[0] = '\0';
      w.rainProbability = today.rainProbability;
      if (w.dayCount >= 2)
        w.tomorrow = w.days[1];
      else
        w.tomorrow.clear();
    }
  }
  glance.valid = w.valid || glance.eventCount > 0;
}

}  // namespace PocketDaily::GlanceFormat
