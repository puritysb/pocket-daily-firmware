#include "WifiSetup.h"

#include <Arduino.h>
#include <esp_attr.h>
namespace Pocket::NearbySync::WifiSetup {
namespace {
RTC_NOINIT_ATTR Ticket ticket;
Route bootRoute = Route::None;
bool restarting = false;
uint32_t restartAt = 0;
}  // namespace
void beginBoot(bool softwareRestart) {
  bootRoute = boot(ticket, softwareRestart);
  if (bootRoute == Route::None && ticket.result == Result::Pending) finish(ticket, Result::Failed);
}
bool wantsJoin() { return bootRoute == Route::Join; }
bool wantsBluetooth() { return bootRoute == Route::Bluetooth; }
const Ticket& current() { return ticket; }
bool receive(const ParsedCommand& command) {
  if (restarting) return false;
  if (!stage(ticket, command)) {
    cancel();
    return false;
  }
  restarting = true;
  restartAt = millis();
  return true;
}
void cancel() {
  erase(&ticket, sizeof(ticket));
  restarting = false;
}
void prepareBluetooth() {
  // No credentials in this HTTP-triggered operation. The authenticated BLE
  // characteristic is the only place that can accept a network/password.
  if (!valid(ticket)) erase(&ticket, sizeof(ticket));
  ticket.route = Route::Bluetooth;
  seal(ticket);
  restarting = true;
  restartAt = millis();
}
bool restartDue() { return restarting && millis() - restartAt >= 500; }
void complete(Result result) {
  finish(ticket, result);
  bootRoute = Route::None;
}
const char* resultName() {
  switch (ticket.result) {
    case Result::None:
      return "none";
    case Result::Pending:
      return "pending";
    case Result::Saved:
      return "saved";
    case Result::Failed:
      return "failed";
    case Result::SaveFailed:
      return "save_failed";
  }
  return "none";
}
}  // namespace Pocket::NearbySync::WifiSetup
