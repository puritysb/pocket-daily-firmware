#pragma once

#include <cstdint>

namespace PocketDaily::RadioHealth {

// Observe association only. An application-port timeout cannot distinguish
// packet loss, filtering or a busy peer from a broken radio; it must not cause
// WiFi.reconnect(). Driver auto-reconnect owns actual association recovery.
class AssociationPolicy {
 public:
  static constexpr uint32_t ABANDON_MS = 5 * 60 * 1000;
  struct Observation {
    bool repaint;
    bool abandon;
  };

  Observation observe(bool connected, uint32_t now) {
    if (connected) {
      const bool recovered = disconnected_;
      disconnected_ = false;
      firstDisconnectAt_ = 0;
      return {recovered, false};
    }
    if (!disconnected_) {
      disconnected_ = true;
      firstDisconnectAt_ = now;
      return {true, false};
    }
    return {false, static_cast<uint32_t>(now - firstDisconnectAt_) > ABANDON_MS};
  }

  bool cleanAssociation() const { return !disconnected_; }

 private:
  uint32_t firstDisconnectAt_ = 0;
  bool disconnected_ = false;
};

}  // namespace PocketDaily::RadioHealth
