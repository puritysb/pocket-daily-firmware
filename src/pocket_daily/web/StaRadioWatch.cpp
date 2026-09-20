#include "pocket_daily/web/StaRadioWatch.h"

#include <Logging.h>
#include <WiFi.h>

#include "pocket_daily/live_studio/NetHealth.h"

namespace PocketDaily::Web {

bool StaRadioWatch::probeGateway() {
  const IPAddress gateway = WiFi.gatewayIP();
  if (gateway == IPAddress(0, 0, 0, 0)) return true;
  for (uint16_t port : {static_cast<uint16_t>(53), static_cast<uint16_t>(80)}) {
    WiFiClient probe;
    probe.setTimeout((PocketDaily::RadioHealth::PROBE_TIMEOUT_MS + 999) / 1000);
    if (probe.connect(gateway, port)) {
      probe.stop();
      return true;
    }
  }
  return false;
}

StaAction StaRadioWatch::onCheck(const wl_status_t wifiStatus, const unsigned long now, const bool serverStable,
                                 bool& repaintOut) {
  bool repaint = false;
  // Driver auto-reconnect handles retries; abandon only after the window, so
  // the activity does not freeze on a blip.
  if (wifiStatus != WL_CONNECTED) {
    if (consecutiveDisconnects == 0) {
      firstDisconnectAt = now;
      repaint = true;
    }
    consecutiveDisconnects++;
    PocketDaily::NetHealth::note("loop_no_wifi", consecutiveDisconnects);
    LOG_DBG("WEBACT", "WiFi not connected (status=%d, consecutive=%d, total=%lu ms)", wifiStatus,
            consecutiveDisconnects, now - firstDisconnectAt);
    if (now - firstDisconnectAt > WIFI_ABANDON_MS) {
      LOG_DBG("WEBACT", "WiFi unavailable for >%lu s; returning to network selection", WIFI_ABANDON_MS / 1000UL);
      return StaAction::Abandon;
    }
    repaintOut = repaint;
    return StaAction::None;
  }

  if (consecutiveDisconnects > 0) {
    LOG_DBG("WEBACT", "WiFi recovered after %d failed checks (%lu ms)", consecutiveDisconnects,
            now - firstDisconnectAt);
    repaint = true;
  }
  consecutiveDisconnects = 0;
  firstDisconnectAt = 0;
  // HN-1: WL_CONNECTED lies in the zombie state - probe the gateway
  // for two-way RF and escalate recovery while deaf. Only once the
  // server is up and stable: during connection setup the activity
  // owns the Wi-Fi state machine and any interference bounces the
  // user back to the home screen (observed 2026-09-20).
  if (serverStable && now - lastRadioProbeMs >= PocketDaily::RadioHealth::PROBE_PERIOD_MS) {
    lastRadioProbeMs = now;
    const bool reachable = probeGateway();
    if (!reachable) PocketDaily::NetHealth::note("probe_dead", 0);
    switch (radioHealth.onProbe(reachable)) {
      case PocketDaily::RadioHealth::Level::DriverReconnect:
        PocketDaily::NetHealth::note("rh_reconnect", 0);
        repaintOut = repaint;
        return StaAction::Reconnect;
      case PocketDaily::RadioHealth::Level::None:
        break;
    }
  }
  repaintOut = repaint;
  return repaint ? StaAction::Repaint : StaAction::None;
}

}  // namespace PocketDaily::Web
