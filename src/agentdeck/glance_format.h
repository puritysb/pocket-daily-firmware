#pragma once
//
// glance_format.h — pure formatting helpers for the sleep-glance dashboard.
// Header-only and Arduino-free so the host ctest suite can pin the strings
// (test/agentdeck_glance). All wall-clock arithmetic here is on the DAEMON's
// local "HH:MM" — the device has no timezone, and the sleep frame's honesty
// rule is absolute times only (they stay true without a repaint).
//
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "glance_state.h"

namespace AgentDeck {
namespace GlanceFormat {

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

// ISO local date → compact weekday label. The daemon owns the timezone and
// sends the already-local date, so the reader needs no timezone database.
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
inline int formatWeatherNow(char* out, size_t cap, const GlanceWeather& w) {
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
inline int formatRainLine(char* out, size_t cap, const GlanceWeather& w) {
  out[0] = '\0';
  if (!w.rainStartHm[0]) return 0;
  int o = snprintf(out, cap, "Rain ~%s", w.rainStartHm);
  if (w.rainEndHm[0]) o += snprintf(out + o, cap - o, "\xE2\x80\x93%s", w.rainEndHm);
  if (w.rainProbability >= 0) o += snprintf(out + o, cap - o, " %d%%", (int)w.rainProbability);
  return o;
}

// "09:30–10:00 Standup" / "09:30 Standup" / "Standup" (all-day, no times).
// Returns chars written; empty when the event has no title.
inline int formatEventLine(char* out, size_t cap, const GlanceEvent& e) {
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
inline int formatTomorrowLine(char* out, size_t cap, const GlanceDayWeather& t) {
  out[0] = '\0';
  if (!t.summary[0] && t.minC == GLANCE_TEMP_NONE) return 0;
  int o = snprintf(out, cap, "Tomorrow");
  if (t.summary[0]) o += snprintf(out + o, cap - o, " %s", t.summary);
  if (t.minC != GLANCE_TEMP_NONE && t.maxC != GLANCE_TEMP_NONE)
    o += snprintf(out + o, cap - o, " %d\xE2\x80\x93%d\xC2\xB0", (int)t.minC, (int)t.maxC);
  if (t.rainProbability > 0) o += snprintf(out + o, cap - o, " \xC2\xB7 rain %d%%", (int)t.rainProbability);
  return o;
}

}  // namespace GlanceFormat
}  // namespace AgentDeck
