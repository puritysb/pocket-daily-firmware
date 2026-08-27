#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace PocketDaily {

struct Choice {
  char id[32];
  char label[41];
};

struct Card {
  char cardId[72];
  char module[8];
  char actionClass[6];
  char title[25];
  char question[161];
  char context[192];
  Choice choices[3];
  uint8_t choiceCount;
};

inline constexpr uint8_t CARD_CAP = 3;
inline constexpr uint8_t WEATHER_DAY_CAP = 5;
inline constexpr int8_t GLANCE_TEMP_NONE = -128;

struct DayWeather {
  char date[11];
  char summary[12];
  int16_t code;
  int8_t minC;
  int8_t maxC;
  int8_t rainProbability;
  void clear() {
    memset(this, 0, sizeof(*this));
    code = -1;
    minC = GLANCE_TEMP_NONE;
    maxC = GLANCE_TEMP_NONE;
    rainProbability = -1;
  }
};

struct Weather {
  bool valid;
  char place[24];
  int16_t code;
  int8_t tempC;
  char summary[12];
  int8_t todayMinC;
  int8_t todayMaxC;
  char rainStartHm[6];
  char rainEndHm[6];
  int8_t rainProbability;
  DayWeather tomorrow;
  DayWeather days[WEATHER_DAY_CAP];
  uint8_t dayCount;
  void clear() {
    memset(this, 0, sizeof(*this));
    code = -1;
    tempC = GLANCE_TEMP_NONE;
    todayMinC = GLANCE_TEMP_NONE;
    todayMaxC = GLANCE_TEMP_NONE;
    rainProbability = -1;
    tomorrow.clear();
    for (auto& day : days) day.clear();
  }
};

struct UsageRow {
  char provider[8];
  char label[12];
  int8_t primaryPercent;
  int8_t secondaryPercent;
  char primaryResetHm[6];
  bool stale;
  void clear() {
    memset(this, 0, sizeof(*this));
    primaryPercent = -1;
    secondaryPercent = -1;
  }
};

struct Event {
  char startHm[6];
  char endHm[6];
  char title[49];
  void clear() { memset(this, 0, sizeof(*this)); }
};

struct Glance {
  static constexpr uint8_t USAGE_CAP = 3;
  static constexpr uint8_t WRAPUP_CAP = 4;
  static constexpr size_t WRAPUP_BYTES = 65;
  static constexpr uint8_t EVENT_CAP = 3;

  bool valid;
  Weather weather;
  UsageRow usage[USAGE_CAP];
  uint8_t usageCount;
  char wrapup[WRAPUP_CAP][WRAPUP_BYTES];
  uint8_t wrapupCount;
  Event events[EVENT_CAP];
  uint8_t eventCount;

  void clear() {
    memset(this, 0, sizeof(*this));
    weather.clear();
    for (auto& row : usage) row.clear();
    for (auto& event : events) event.clear();
  }
};

}  // namespace PocketDaily
