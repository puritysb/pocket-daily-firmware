#pragma once

#include <cstdint>

// HN-1 radio self-healing policy (docs/network-stability-analysis.md).
// Pure escalation state machine; the activity loop feeds it gateway-probe
// results and applies the returned recovery level.
//
// Detection: a TCP connect to the gateway proves two-way RF (either a
// handshake or an RST). `WL_CONNECTED` alone lies in the zombie state.
// Recovery escalates only while the radio stays deaf; any successful probe
// fully resets. Levels: 0 none, 1 WiFi.reconnect(), 2 full disconnect +
// re-begin with saved credentials, 3 esp_wifi_stop/start (deepest driver
// reset short of reboot). Level 3 repeats until the activity's existing
// abandon path takes over.
namespace PocketDaily::RadioHealth {

inline constexpr uint32_t PROBE_PERIOD_MS = 10 * 1000;
inline constexpr uint32_t PROBE_TIMEOUT_MS = 1000;
inline constexpr uint8_t STRIKES_PER_LEVEL = 3;

enum class Level : uint8_t {
  None = 0,
  DriverReconnect = 1,  // WiFi.reconnect() - safe under the Arduino layer
  // Intentionally no deeper level: disconnect+begin fights the activity's
  // own Wi-Fi state machine (bounces File Transfer to the home screen) and
  // esp_wifi_stop/start crashes the Arduino stack (observed 2026-09-20).
  // Deeper recovery belongs to a guided reboot, not the radio ladder.
};

class Policy {
 public:
  // Feed one probe result; returns the recovery action to apply now
  // (None usually; an escalation level once strikes accumulate). A reachable
  // probe fully heals: strikes AND level reset - it proves the radio works,
  // so no further escalation is warranted.
  Level onProbe(bool reachable) {
    if (reachable) {
      strikes_ = 0;
      level_ = Level::None;
      return Level::None;
    }
    strikes_++;
    if (strikes_ < STRIKES_PER_LEVEL) return Level::None;
    strikes_ = 0;
    const Level next = escalate();
    return next;
  }

  Level currentLevel() const { return level_; }

  void reset() {
    strikes_ = 0;
    level_ = Level::None;
  }

 private:
  Level escalate() {
    if (level_ == Level::None) level_ = Level::DriverReconnect;
    return level_;
  }

  uint8_t strikes_ = 0;
  Level level_ = Level::None;
};

}  // namespace PocketDaily::RadioHealth
