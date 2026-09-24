#include "pocket_daily/web/StaRadioWatch.h"

#include <Logging.h>

#include "pocket_daily/live_studio/NetHealth.h"

namespace PocketDaily::Web {

StaAction StaRadioWatch::onCheck(const wl_status_t wifiStatus, const unsigned long now, bool& repaintOut) {
  const auto observation = association.observe(wifiStatus == WL_CONNECTED, static_cast<uint32_t>(now));
  repaintOut = observation.repaint;
  if (observation.abandon) {
    PocketDaily::NetHealth::note("association_abandon", wifiStatus);
    LOG_DBG("WEBACT", "WiFi association unavailable for >5 minutes; leaving File Transfer");
    return StaAction::Abandon;
  }
  if (observation.repaint) {
    PocketDaily::NetHealth::note("association_change", wifiStatus);
    return StaAction::Repaint;
  }
  return StaAction::None;
}

}  // namespace PocketDaily::Web
