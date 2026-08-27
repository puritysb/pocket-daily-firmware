#include <gtest/gtest.h>

#include "src/agentdeck/glance_format.h"
#include "src/agentdeck/glance_state.h"

using namespace AgentDeck;
using namespace AgentDeck::GlanceFormat;

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

TEST(GlanceDate, FormatsWeekdayFromDaemonLocalDate) {
  char out[4];
  EXPECT_TRUE(formatWeekday(out, sizeof(out), "2026-08-24"));
  EXPECT_STREQ(out, "MON");
  EXPECT_TRUE(formatWeekday(out, sizeof(out), "2026-08-30"));
  EXPECT_STREQ(out, "SUN");
  EXPECT_FALSE(formatWeekday(out, sizeof(out), "2026/08/24"));
  EXPECT_STREQ(out, "");
}

GlanceWeather weather() {
  GlanceWeather w;
  w.clear();
  w.valid = true;
  w.code = 61;
  w.tempC = 28;
  snprintf(w.summary, sizeof(w.summary), "Rain");
  w.todayMinC = 22;
  w.todayMaxC = 30;
  return w;
}

TEST(GlanceWeatherFormat, NowLineComposes) {
  char buf[96];
  GlanceWeather w = weather();
  formatWeatherNow(buf, sizeof(buf), w);
  EXPECT_STREQ(buf,
               "28\xC2\xB0 Rain \xC2\xB7 22\xE2\x80\x93"
               "30\xC2\xB0");
}

TEST(GlanceWeatherFormat, NowLinePartsDropOut) {
  char buf[96];
  GlanceWeather w;
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

TEST(GlanceWeatherFormat, RainWindow) {
  char buf[96];
  GlanceWeather w = weather();
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

TEST(GlanceWeatherFormat, TomorrowLine) {
  char buf[96];
  GlanceDayWeather t;
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
  GlanceInfo g;
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
  AgentDeck::GlanceEvent e;
  e.clear();
  snprintf(e.title, sizeof(e.title), "Standup");
  char buf[96];
  AgentDeck::GlanceFormat::formatEventLine(buf, sizeof(buf), e);
  EXPECT_STREQ(buf, "Standup");  // all-day: no times

  snprintf(e.startHm, sizeof(e.startHm), "09:30");
  AgentDeck::GlanceFormat::formatEventLine(buf, sizeof(buf), e);
  EXPECT_STREQ(buf, "09:30 Standup");

  snprintf(e.endHm, sizeof(e.endHm), "10:00");
  AgentDeck::GlanceFormat::formatEventLine(buf, sizeof(buf), e);
  EXPECT_STREQ(buf,
               "09:30\xE2\x80\x93"
               "10:00 Standup");

  e.title[0] = '\0';
  EXPECT_EQ(AgentDeck::GlanceFormat::formatEventLine(buf, sizeof(buf), e), 0);
  EXPECT_STREQ(buf, "");
}
