#pragma once

// Compatibility view for the AgentDeck provider adapter. Pocket Daily owns
// the bounded weather/schedule models; keeping these aliases avoids a wire or
// cache schema break while provider code is migrated incrementally.
#include "pocket_daily/models.h"

namespace AgentDeck {

inline constexpr int8_t GLANCE_TEMP_NONE = PocketDaily::GLANCE_TEMP_NONE;
using GlanceDayWeather = PocketDaily::DayWeather;
using GlanceWeather = PocketDaily::Weather;
using GlanceUsageRow = PocketDaily::UsageRow;
using GlanceEvent = PocketDaily::Event;
using GlanceInfo = PocketDaily::Glance;

}  // namespace AgentDeck
