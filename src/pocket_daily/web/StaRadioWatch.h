#pragma once

#include <WiFi.h>

#include <cstdint>

#include "pocket_daily/web/RadioHealthPolicy.h"

namespace PocketDaily::Web {

// Passive actions only; a liveness check cannot tear down the radio.
enum class StaAction : uint8_t {
  None,
  Repaint,
  Abandon,
};

// No gateway socket, blocking probe or forced reconnect. The driver owns
// association retries; the activity leaves only after sustained disconnection.
class StaRadioWatch final {
 public:
  // Always sets repaintOut, including on the abandon path.
  StaAction onCheck(wl_status_t wifiStatus, unsigned long now, bool& repaintOut);
  bool cleanAssociation() const { return association.cleanAssociation(); }

 private:
  PocketDaily::RadioHealth::AssociationPolicy association;
};

}  // namespace PocketDaily::Web
