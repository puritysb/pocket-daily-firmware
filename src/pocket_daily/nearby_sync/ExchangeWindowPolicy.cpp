#include "ExchangeWindowPolicy.h"

namespace Pocket::NearbySync::Window {
namespace {
// Wrap-safe "a is at or after b" for millis() values.
bool reached(const uint32_t now, const uint32_t when) { return static_cast<int32_t>(now - when) >= 0; }
}  // namespace

uint32_t durationMs(const Trigger trigger) {
  switch (trigger) {
    case Trigger::BOOK_CLOSED:
      return BOOK_CLOSED_MS;
    case Trigger::WAKE:
      return WAKE_MS;
    case Trigger::SLEEP:
      return SLEEP_MS;
  }
  return SLEEP_MS;
}

Gate evaluate(const GateInput& input) {
  if (!input.enabled) return Gate::SETTING_OFF;
  if (!input.bonded) return Gate::NO_BOND;
  if (input.batteryPercent <= MIN_BATTERY_PERCENT) return Gate::LOW_BATTERY;
  if (input.freeHeap < BLE_WINDOW_MIN_FREE || input.largestBlock < BLE_WINDOW_MIN_BLOCK) return Gate::LOW_MEMORY;
  return Gate::OPEN;
}

bool readyAllowed(const uint32_t freeHeap, const uint32_t largestBlock) {
  return freeHeap >= BLE_WINDOW_READY_MIN_FREE && largestBlock >= BLE_WINDOW_READY_MIN_BLOCK;
}

const char* triggerName(const Trigger trigger) {
  switch (trigger) {
    case Trigger::BOOK_CLOSED:
      return "book-closed";
    case Trigger::WAKE:
      return "wake";
    case Trigger::SLEEP:
      return "sleep";
  }
  return "?";
}

const char* gateName(const Gate gate) {
  switch (gate) {
    case Gate::OPEN:
      return "open";
    case Gate::NO_BOND:
      return "no-bond";
    case Gate::SETTING_OFF:
      return "disabled";
    case Gate::LOW_BATTERY:
      return "low-battery";
    case Gate::LOW_MEMORY:
      return "low-memory";
  }
  return "?";
}

const char* closeReasonName(const CloseReason reason) {
  switch (reason) {
    case CloseReason::TIME_UP:
      return "time-up";
    case CloseReason::BOOK_OPENED:
      return "book-opened";
    case CloseReason::RADIO_OWNER:
      return "radio-owner";
    case CloseReason::LEFT_SHELL:
      return "left-shell";
    case CloseReason::BUTTON:
      return "button";
    case CloseReason::START_FAILED:
      return "start-failed";
    case CloseReason::LOW_MEMORY:
      return "low-memory";
  }
  return "?";
}

void Controller::arm(const Trigger next, const uint32_t nowMs, const uint32_t renderCount) {
  if (radioUp()) {
    const uint32_t extended = nowMs + durationMs(next);
    if (next == Trigger::SLEEP || reached(extended, deadline)) deadline = extended;
    armedTrigger = next;
    return;
  }
  current = State::ARMED;
  armedTrigger = next;
  armedAt = nowMs;
  armedRender = renderCount;
}

Controller::Action Controller::tick(const uint32_t nowMs, const uint32_t renderCount, const bool shellActive) {
  switch (current) {
    case State::IDLE:
      return Action::NONE;
    case State::ARMED: {
      if (!shellActive) {
        current = State::IDLE;
        closeReason = CloseReason::LEFT_SHELL;
        return Action::NONE;
      }
      const bool frameDone = renderCount != armedRender;
      if (armedTrigger != Trigger::SLEEP && !frameDone && !reached(nowMs, armedAt + RENDER_WAIT_MS)) {
        return Action::NONE;
      }
      current = State::STARTING;
      deadline = nowMs + durationMs(armedTrigger);
      return Action::START;
    }
    case State::STARTING:
    case State::OPEN:
      if (!shellActive) return close(CloseReason::LEFT_SHELL);
      if (reached(nowMs, deadline)) return close(CloseReason::TIME_UP);
      return Action::NONE;
  }
  return Action::NONE;
}

void Controller::started(const bool ok, const uint32_t nowMs) {
  if (current != State::STARTING) return;
  if (!ok) {
    current = State::IDLE;
    closeReason = CloseReason::START_FAILED;
    return;
  }
  current = State::OPEN;
  // A start that took long still honours the original deadline.
  if (reached(nowMs, deadline)) deadline = nowMs;
}

Controller::Action Controller::close(const CloseReason reason) {
  closeReason = reason;
  if (radioUp()) return Action::STOP;
  current = State::IDLE;
  return Action::NONE;
}

void Controller::stopped() { current = State::IDLE; }

uint32_t Controller::remainingMs(const uint32_t nowMs) const {
  if (!radioUp() || reached(nowMs, deadline)) return 0;
  return deadline - nowMs;
}

}  // namespace Pocket::NearbySync::Window
