#include <gtest/gtest.h>

#include "src/pocket_daily/home/GlanceFormat.h"

using namespace PocketDaily;
using namespace PocketDaily::GlanceFormat;

namespace {

TEST(GlanceHm, ParsesAndRejects) {
  EXPECT_EQ(hmToMinutes("00:00"), 0);
  EXPECT_EQ(hmToMinutes("14:32"), 14 * 60 + 32);
  EXPECT_EQ(hmToMinutes("23:59"), 23 * 60 + 59);
  EXPECT_EQ(hmToMinutes("24:00"), -1);
  EXPECT_EQ(hmToMinutes("14:60"), -1);
  EXPECT_EQ(hmToMinutes("1432"), -1);
  EXPECT_EQ(hmToMinutes("ab:cd"), -1);
  EXPECT_EQ(hmToMinutes(""), -1);
  EXPECT_EQ(hmToMinutes(nullptr), -1);
}

TEST(GlanceHm, AddWrapsAcrossMidnight) {
  char out[6];
  ASSERT_TRUE(addToHm(out, sizeof(out), "14:32", 3600));
  EXPECT_STREQ(out, "15:32");
  ASSERT_TRUE(addToHm(out, sizeof(out), "23:50", 900));
  EXPECT_STREQ(out, "00:05");
  ASSERT_TRUE(addToHm(out, sizeof(out), "00:00", 0));
  EXPECT_STREQ(out, "00:00");
  EXPECT_FALSE(addToHm(out, sizeof(out), "garbage", 60));
}

TEST(GlanceDate, FormatsWeekdayFromAppLocalDate) {
  char out[4];
  EXPECT_TRUE(formatWeekday(out, sizeof(out), "2026-08-24"));
  EXPECT_STREQ(out, "MON");
  EXPECT_TRUE(formatWeekday(out, sizeof(out), "2026-08-30"));
  EXPECT_STREQ(out, "SUN");
  EXPECT_FALSE(formatWeekday(out, sizeof(out), "2026/08/24"));
  EXPECT_STREQ(out, "");
}

Weather weather() {
  Weather w;
  w.clear();
  w.valid = true;
  w.code = 61;
  w.tempC = 28;
  snprintf(w.summary, sizeof(w.summary), "Rain");
  w.todayMinC = 22;
  w.todayMaxC = 30;
  return w;
}

TEST(WeatherFormat, NowLineComposes) {
  char buf[96];
  Weather w = weather();
  formatWeatherNow(buf, sizeof(buf), w);
  EXPECT_STREQ(buf,
               "28\xC2\xB0 Rain \xC2\xB7 22\xE2\x80\x93"
               "30\xC2\xB0");
}

TEST(WeatherFormat, NowLinePartsDropOut) {
  char buf[96];
  Weather w;
  w.clear();
  w.valid = true;
  w.tempC = 5;
  formatWeatherNow(buf, sizeof(buf), w);
  EXPECT_STREQ(buf, "5\xC2\xB0");
  w.tempC = GLANCE_TEMP_NONE;
  snprintf(w.summary, sizeof(w.summary), "Clear");
  formatWeatherNow(buf, sizeof(buf), w);
  EXPECT_STREQ(buf, "Clear");
}

TEST(WeatherFormat, RainWindow) {
  char buf[96];
  Weather w = weather();
  formatRainLine(buf, sizeof(buf), w);
  EXPECT_STREQ(buf, "");  // no window set
  snprintf(w.rainStartHm, sizeof(w.rainStartHm), "15:00");
  w.rainProbability = 70;
  formatRainLine(buf, sizeof(buf), w);
  EXPECT_STREQ(buf, "Rain ~15:00 70%");
  snprintf(w.rainEndHm, sizeof(w.rainEndHm), "17:00");
  formatRainLine(buf, sizeof(buf), w);
  EXPECT_STREQ(buf,
               "Rain ~15:00\xE2\x80\x93"
               "17:00 70%");
}

TEST(WeatherFormat, TomorrowLine) {
  char buf[96];
  DayWeather t;
  t.clear();
  formatTomorrowLine(buf, sizeof(buf), t);
  EXPECT_STREQ(buf, "");
  snprintf(t.summary, sizeof(t.summary), "Clear");
  t.minC = 22;
  t.maxC = 31;
  t.rainProbability = 10;
  formatTomorrowLine(buf, sizeof(buf), t);
  EXPECT_STREQ(buf,
               "Tomorrow Clear 22\xE2\x80\x93"
               "31\xC2\xB0 \xC2\xB7 rain 10%");
}

TEST(GlanceState, ClearRestoresSentinels) {
  Glance g;
  memset(&g, 0x5a, sizeof(g));
  g.clear();
  EXPECT_FALSE(g.valid);
  EXPECT_EQ(g.weather.tempC, GLANCE_TEMP_NONE);
  EXPECT_EQ(g.weather.code, -1);
  EXPECT_EQ(g.weather.rainProbability, -1);
  EXPECT_EQ(g.weather.tomorrow.minC, GLANCE_TEMP_NONE);
  EXPECT_EQ(g.weather.tomorrow.code, -1);
  EXPECT_EQ(g.weather.dayCount, 0);
  EXPECT_EQ(g.weather.days[0].minC, GLANCE_TEMP_NONE);
  EXPECT_EQ(g.usage[0].primaryPercent, -1);
  EXPECT_EQ(g.usageCount, 0);
  EXPECT_EQ(g.wrapupCount, 0);
}

}  // namespace

TEST(GlanceEventFormat, EventLineComposes) {
  PocketDaily::Event e;
  e.clear();
  snprintf(e.title, sizeof(e.title), "Standup");
  char buf[96];
  PocketDaily::GlanceFormat::formatEventLine(buf, sizeof(buf), e);
  EXPECT_STREQ(buf, "Standup");  // all-day: no times

  snprintf(e.startHm, sizeof(e.startHm), "09:30");
  PocketDaily::GlanceFormat::formatEventLine(buf, sizeof(buf), e);
  EXPECT_STREQ(buf, "09:30 Standup");

  snprintf(e.endHm, sizeof(e.endHm), "10:00");
  PocketDaily::GlanceFormat::formatEventLine(buf, sizeof(buf), e);
  EXPECT_STREQ(buf,
               "09:30\xE2\x80\x93"
               "10:00 Standup");

  e.title[0] = '\0';
  EXPECT_EQ(PocketDaily::GlanceFormat::formatEventLine(buf, sizeof(buf), e), 0);
  EXPECT_STREQ(buf, "");
}

TEST(GlanceDate, ValidatesIsoDates) {
  EXPECT_TRUE(validIsoDate("2026-09-25"));
  EXPECT_TRUE(validIsoDate("1970-01-01"));
  EXPECT_FALSE(validIsoDate("2026-13-01"));
  EXPECT_FALSE(validIsoDate("2026-00-10"));
  EXPECT_FALSE(validIsoDate("2026-09-32"));
  EXPECT_FALSE(validIsoDate("1969-12-31"));
  EXPECT_FALSE(validIsoDate("2026-9-25"));
  EXPECT_FALSE(validIsoDate("2026/09/25"));
  EXPECT_FALSE(validIsoDate(""));
  EXPECT_FALSE(validIsoDate(nullptr));
}

TEST(GlanceDate, FormatsLocalDateWithTheAppOffset) {
  char out[11];
  // 2026-09-25T15:30:00Z.
  constexpr int64_t epoch = 1790350200;
  ASSERT_TRUE(formatLocalIsoDate(out, sizeof(out), epoch, 0));
  EXPECT_STREQ(out, "2026-09-25");
  ASSERT_TRUE(formatLocalIsoDate(out, sizeof(out), epoch, 540));  // Seoul: 00:30 next day
  EXPECT_STREQ(out, "2026-09-26");
  ASSERT_TRUE(formatLocalIsoDate(out, sizeof(out), epoch, -720));  // UTC-12: 03:30 same day
  EXPECT_STREQ(out, "2026-09-25");
  ASSERT_TRUE(formatLocalIsoDate(out, sizeof(out), 951782400, 0));  // leap day 2000-02-29
  EXPECT_STREQ(out, "2000-02-29");
  EXPECT_FALSE(formatLocalIsoDate(out, sizeof(out), 0, -60));
  EXPECT_FALSE(formatLocalIsoDate(out, 10, epoch, 0));
}

namespace {
Glance forecast() {
  Glance g;
  g.clear();
  g.valid = true;
  Weather& w = g.weather;
  w.valid = true;
  snprintf(w.place, sizeof(w.place), "Seoul");
  w.code = 61;
  w.tempC = 18;
  snprintf(w.summary, sizeof(w.summary), "Rain");
  w.todayMinC = 12;
  w.todayMaxC = 21;
  snprintf(w.rainStartHm, sizeof(w.rainStartHm), "14:00");
  w.rainProbability = 80;
  const char* dates[] = {"2026-09-25", "2026-09-26", "2026-09-27"};
  for (int i = 0; i < 3; ++i) {
    snprintf(w.days[i].date, sizeof(w.days[i].date), "%s", dates[i]);
    snprintf(w.days[i].summary, sizeof(w.days[i].summary), "D%d", i);
    w.days[i].code = static_cast<int16_t>(i);
    w.days[i].minC = static_cast<int8_t>(10 + i);
    w.days[i].maxC = static_cast<int8_t>(20 + i);
    w.days[i].rainProbability = static_cast<int8_t>(10 * i);
  }
  w.dayCount = 3;
  w.tomorrow = w.days[1];
  snprintf(g.events[0].title, sizeof(g.events[0].title), "Standup");
  g.eventCount = 1;
  return g;
}
}  // namespace

TEST(GlanceRoll, SameDayOrClockBehindLeavesTheGlanceAlone) {
  const Glance before = forecast();
  Glance g = before;
  rollToLocalDay(g, "2026-09-25", "2026-09-25");
  EXPECT_EQ(memcmp(&g, &before, sizeof(g)), 0);
  rollToLocalDay(g, "2026-09-25", "2026-09-24");
  EXPECT_EQ(memcmp(&g, &before, sizeof(g)), 0);
  rollToLocalDay(g, "garbage", "2026-09-27");
  EXPECT_EQ(memcmp(&g, &before, sizeof(g)), 0);
}

TEST(GlanceRoll, NextDayDropsTheScheduleAndPastForecast) {
  Glance g = forecast();
  rollToLocalDay(g, "2026-09-25", "2026-09-26");
  EXPECT_TRUE(g.valid);
  EXPECT_EQ(g.eventCount, 0);
  EXPECT_EQ(g.events[0].title[0], '\0');
  ASSERT_TRUE(g.weather.valid);
  ASSERT_EQ(g.weather.dayCount, 2);
  EXPECT_STREQ(g.weather.days[0].date, "2026-09-26");
  EXPECT_STREQ(g.weather.days[1].date, "2026-09-27");
  EXPECT_EQ(g.weather.days[2].date[0], '\0');
  EXPECT_EQ(g.weather.days[2].minC, GLANCE_TEMP_NONE);
  // The "now" block describes today's forecast day, not yesterday's hour.
  EXPECT_EQ(g.weather.tempC, GLANCE_TEMP_NONE);
  EXPECT_EQ(g.weather.code, 1);
  EXPECT_STREQ(g.weather.summary, "D1");
  EXPECT_EQ(g.weather.todayMinC, 11);
  EXPECT_EQ(g.weather.todayMaxC, 21);
  EXPECT_EQ(g.weather.rainStartHm[0], '\0');
  EXPECT_EQ(g.weather.rainProbability, 10);
  EXPECT_STREQ(g.weather.tomorrow.date, "2026-09-27");
  // Idempotent on the same day.
  const Glance once = g;
  rollToLocalDay(g, "2026-09-25", "2026-09-26");
  EXPECT_EQ(memcmp(&g, &once, sizeof(g)), 0);
}

TEST(GlanceRoll, LastForecastDayHasNoTomorrow) {
  Glance g = forecast();
  rollToLocalDay(g, "2026-09-25", "2026-09-27");
  ASSERT_TRUE(g.weather.valid);
  EXPECT_EQ(g.weather.dayCount, 1);
  EXPECT_EQ(g.weather.tomorrow.date[0], '\0');
}

TEST(GlanceRoll, ForecastInThePastLosesTheWeather) {
  Glance g = forecast();
  rollToLocalDay(g, "2026-09-25", "2026-10-02");
  EXPECT_FALSE(g.weather.valid);
  EXPECT_EQ(g.weather.tempC, GLANCE_TEMP_NONE);
  EXPECT_EQ(g.eventCount, 0);
  EXPECT_FALSE(g.valid);
}
