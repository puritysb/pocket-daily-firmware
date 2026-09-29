#pragma once

#include <cstddef>
#include <cstdint>

// Exchange windows of Pocket Reading Sync over BLE v1
// (docs/reading-sync-ble-v1.md): when the reader advertises the bonded Nearby
// Sync service on its own, for how long, and what closes it. Pure so the host
// tests drive the same decisions; ExchangeWindow applies them to NimBLE.
namespace Pocket::NearbySync::Window {

enum class Trigger : uint8_t { BOOK_CLOSED, WAKE, SLEEP };

inline constexpr uint32_t BOOK_CLOSED_MS = 45000;
inline constexpr uint32_t WAKE_MS = 45000;
inline constexpr uint32_t SLEEP_MS = 20000;
uint32_t durationMs(Trigger trigger);

// A window waits for the shell's next completed frame so the radio never delays
// what the person is looking at; it opens anyway after this long.
inline constexpr uint32_t RENDER_WAIT_MS = 5000;
// A peer that connects with a resolvable private address the controller could
// not resolve gets this long to re-encrypt with its bond before it is dropped.
inline constexpr uint32_t UNBONDED_GRACE_MS = 5000;

// One disconnect request per unauthenticated connection. Starts before the
// startup worker advertises, so a connection during init gets the same limit.
class UnbondedGrace {
 public:
  bool expired(uint32_t generation, uint32_t connectedAtMs, uint32_t nowMs);

 private:
  uint32_t droppedGeneration = 0;
};

// Provisional safety budget: the historical X3 window consumed ~70 KiB before
// any phone connected. Reserve startup overhead plus 24 KiB for the exchange;
// do not lower these gates merely to make advertising start on a busy shell.
// X3/X4 acceptance on the current SDK is still required.
inline constexpr uint32_t BLE_WINDOW_MIN_BLOCK = 40U * 1024U;
inline constexpr uint32_t BLE_WINDOW_MIN_FREE = 96U * 1024U;
inline constexpr uint32_t BLE_WINDOW_READY_MIN_FREE = 24U * 1024U;
inline constexpr uint32_t BLE_WINDOW_READY_MIN_BLOCK = 8U * 1024U;
inline constexpr uint32_t BLE_WINDOW_RUNNING_MIN_FREE = 20U * 1024U;
inline constexpr uint32_t BLE_WINDOW_RUNNING_MIN_BLOCK = 4U * 1024U;
// The window needs battery strictly above this.
inline constexpr uint16_t MIN_BATTERY_PERCENT = 10;

enum class Gate : uint8_t { OPEN, NO_BOND, SETTING_OFF, LOW_BATTERY, LOW_MEMORY };

struct GateInput {
  bool bonded = false;   // at least one BLE bond is stored
  bool enabled = false;  // Settings: "Sync places with your phone"
  uint16_t batteryPercent = 0;
  uint32_t freeHeap = 0;
  uint32_t largestBlock = 0;
};

Gate evaluate(const GateInput& input);
bool readyAllowed(uint32_t freeHeap, uint32_t largestBlock);
bool runningAllowed(uint32_t freeHeap, uint32_t largestBlock);

enum class CloseReason : uint8_t {
  TIME_UP,
  BOOK_OPENED,
  RADIO_OWNER,  // a Wi-Fi mode or the Nearby Sync screen takes the radio
  LEFT_SHELL,   // any other screen or dialog, which may need the heap
  BUTTON,       // a press during the sleep window
  START_FAILED,
  LOW_MEMORY,
};

const char* triggerName(Trigger trigger);
const char* gateName(Gate gate);
const char* closeReasonName(CloseReason reason);

// The window lifecycle. IDLE → ARMED (trigger seen) → STARTING (radio coming
// up in a worker) → OPEN (advertising / connected) → IDLE. The owner performs
// the START and STOP actions and reports back.
class Controller {
 public:
  enum class State : uint8_t { IDLE, ARMED, STARTING, OPEN };
  enum class Action : uint8_t { NONE, START, STOP };

  // A trigger on the shell. BOOK_CLOSED and WAKE wait for the next completed
  // frame (renderCount advancing); SLEEP starts at once (its frame is already
  // on the panel). A trigger while the radio is up extends the deadline: SLEEP
  // to exactly SLEEP_MS from now, the others to at least their duration.
  void arm(Trigger next, uint32_t nowMs, uint32_t renderCount);
  // One owner loop pass. `shellActive`: the current screen tolerates the
  // radio (Home, Pocket Daily, Library, Recent Books, the sleep screen).
  Action tick(uint32_t nowMs, uint32_t renderCount, bool shellActive);
  // STARTING → OPEN, or IDLE when the gates, allocation or NimBLE failed.
  void started(bool ok, uint32_t nowMs);
  // Close now. STOP when the radio is up or starting (the owner must stop it
  // before the next screen enters), NONE when nothing was running.
  Action close(CloseReason reason);
  // The owner released NimBLE.
  void stopped();

  State state() const { return current; }
  Trigger trigger() const { return armedTrigger; }
  CloseReason lastCloseReason() const { return closeReason; }
  bool radioUp() const { return current == State::STARTING || current == State::OPEN; }
  // Milliseconds until the deadline (0 when not up or already due).
  uint32_t remainingMs(uint32_t nowMs) const;

 private:
  State current = State::IDLE;
  Trigger armedTrigger = Trigger::BOOK_CLOSED;
  CloseReason closeReason = CloseReason::TIME_UP;
  uint32_t armedAt = 0;
  uint32_t armedRender = 0;
  uint32_t deadline = 0;
};

}  // namespace Pocket::NearbySync::Window
