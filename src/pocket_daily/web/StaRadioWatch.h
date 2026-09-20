#pragma once

#include <WiFi.h>

#include <cstdint>

#include "pocket_daily/web/RadioHealthPolicy.h"

namespace PocketDaily::Web {

// What the activity should do after one radio-ladder check.
enum class StaAction : uint8_t {
  None,       // nothing changed
  Repaint,    // association state flipped; redraw the header indicator
  Reconnect,  // gateway probe escalated to WiFi.reconnect()
  Abandon,    // association lost longer than the abandon window; leave
};

// STA radio ladder for the File Transfer server-running loop
// (docs/network-stability-analysis.md): sustained-loss tracking with a
// bounded abandon, plus the two-way gateway probe that catches the zombie
// `WL_CONNECTED` state. Owns the RadioHealth escalation policy; the activity
// feeds it the WiFi status every ~2 s and applies the returned action.
class StaRadioWatch final {
 public:
  // Returns the action to apply; `repaintOut` is set independently when the
  // association state flipped this check (recovery and a reconnect escalation
  // can coincide, and the original loop repainted for both).
  StaAction onCheck(wl_status_t wifiStatus, unsigned long now, bool serverStable, bool& repaintOut);

  // False while any recent check saw the association down; the WiFi indicator
  // renders its "down" cross until a check observes the recovery.
  bool cleanAssociation() const { return consecutiveDisconnects == 0; }

 private:
  // Two-way RF proof: a TCP handshake with the gateway on either of two
  // near-universally-open ports (DNS, router admin). A single boolean connect
  // cannot distinguish RST from timeout, so both ports are tried; a router
  // with neither open is treated as alive to avoid false escalations.
  bool probeGateway();

  PocketDaily::RadioHealth::Policy radioHealth;
  unsigned long lastRadioProbeMs = 0;
  int consecutiveDisconnects = 0;
  unsigned long firstDisconnectAt = 0;
  // Sustained WiFi-loss tracking; abandon only after this window.
  static constexpr unsigned long WIFI_ABANDON_MS = 5UL * 60UL * 1000UL;
};

}  // namespace PocketDaily::Web
