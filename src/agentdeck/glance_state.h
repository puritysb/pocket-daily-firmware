#pragma once
//
// glance_state.h — bounded storage for the card-feed `glance` block (the sleep
// dashboard content). Contract: AgentDeck shared/src/protocol.ts § Glance +
// docs/esp32-client-contract.md § Pull sync.
//
// The daemon pre-renders and byte-trims every string; these caps mirror the
// contract's budgets (wrapup lines ≤ 64 UTF-8 bytes, ≤ 4 lines, ≤ 3 usage
// rows). Everything lives inside DashboardState / DeckStore::Snapshot, so the
// struct must stay POD, fixed-size, and memset-safe — clear() restores the
// "no data" sentinels that a plain zero-fill would get wrong (0°C is valid).
//
#include <cstdint>
#include <cstring>

namespace AgentDeck {

// Sentinel for "no temperature reported" (°C values are small integers).
static constexpr int8_t GLANCE_TEMP_NONE = -128;

struct GlanceDayWeather {
  char summary[12];  // "Clear" / "Rain" / … (ASCII, daemon-rendered)
  int8_t minC;
  int8_t maxC;
  int8_t rainProbability;  // 0-100, -1 = not reported
  void clear() {
    memset(this, 0, sizeof(*this));
    minC = GLANCE_TEMP_NONE;
    maxC = GLANCE_TEMP_NONE;
    rainProbability = -1;
  }
};

struct GlanceWeather {
  bool valid;  // any weather content arrived
  char place[24];
  int8_t tempC;  // current, GLANCE_TEMP_NONE = none
  char summary[12];
  int8_t todayMinC;
  int8_t todayMaxC;
  // Today's next rain window (absolute daemon-local HH:MM). Empty = no rain.
  char rainStartHm[6];
  char rainEndHm[6];
  int8_t rainProbability;  // peak inside the window, -1 = none
  GlanceDayWeather tomorrow;
  void clear() {
    memset(this, 0, sizeof(*this));
    tempC = GLANCE_TEMP_NONE;
    todayMinC = GLANCE_TEMP_NONE;
    todayMaxC = GLANCE_TEMP_NONE;
    rainProbability = -1;
    tomorrow.clear();
  }
};

struct GlanceUsageRow {
  char provider[8];  // "claude" / "codex"
  char label[12];    // "Claude" / "Codex"
  int8_t primaryPercent;    // 5h used %, -1 = none
  int8_t secondaryPercent;  // 7d used %, -1 = none
  char primaryResetHm[6];   // absolute daemon-local "HH:MM", "" = none
  bool stale;
  void clear() {
    memset(this, 0, sizeof(*this));
    primaryPercent = -1;
    secondaryPercent = -1;
  }
};

struct GlanceInfo {
  static constexpr uint8_t USAGE_CAP = 3;
  static constexpr uint8_t WRAPUP_CAP = 4;
  static constexpr size_t WRAPUP_BYTES = 65;  // 64-byte budget + NUL

  bool valid;  // a glance block arrived at least once
  GlanceWeather weather;
  GlanceUsageRow usage[USAGE_CAP];
  uint8_t usageCount;
  char wrapup[WRAPUP_CAP][WRAPUP_BYTES];
  uint8_t wrapupCount;

  void clear() {
    memset(this, 0, sizeof(*this));
    weather.clear();
    for (auto& u : usage) u.clear();
  }
};

}  // namespace AgentDeck
