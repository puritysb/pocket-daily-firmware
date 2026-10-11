#include "DevNetworkFault.h"
#ifdef ENABLE_DEV_REMOTE_FLASH
#include <Arduino.h>
#include <WiFi.h>

namespace PocketDaily::Web::DevNetworkFault {
namespace {
Trial trial;
bool autoReconnect = true;
}  // namespace
const Trial& evidence() { return trial; }
bool arm(uint32_t run, uint32_t size, Point point) {
  if (WiFi.status() != WL_CONNECTED) return false;
  return trial.arm(run, size, point, millis());
}
bool trigger(Point point, uint32_t size, uint32_t received) {
  if (trial.run && trial.size == size && point == Point::Published) ++trial.publications;
  if (!trial.trigger(point, size, received, millis())) return false;
  autoReconnect = WiFi.getAutoReconnect();
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, false);
  return true;
}
void cancel() {
  if (trial.state == State::Offline || trial.state == State::Recovering) WiFi.setAutoReconnect(autoReconnect);
  if (trial.state != State::Complete) trial.state = State::Expired;
}
void service() {
  if (trial.tick(millis(), WiFi.status() == WL_CONNECTED)) {
    WiFi.setAutoReconnect(autoReconnect);
    WiFi.reconnect();
  }
}
}  // namespace PocketDaily::Web::DevNetworkFault
#endif
