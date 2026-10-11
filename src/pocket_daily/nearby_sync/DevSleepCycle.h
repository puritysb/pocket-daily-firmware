#pragma once

#ifdef ENABLE_DEV_REMOTE_FLASH
#include <cstdint>

#include "ReadSyncStats.h"

// Explicit LAN-requested developer experiment; never a production boot mode.
namespace Pocket::NearbySync::DevSleepCycle {
enum class State : uint32_t { Requested = 1, Running, Completed, Interrupted };
enum class ReturnMode : uint32_t { Restart = 0, TimedDeepSleep = 1, AppStandby = 2 };
enum class Checkpoint : uint32_t { None, WifiEntry, FrameReleased, BeforeLightSleep, RadioReady, StandbyAlive };
struct Record {
  uint32_t magic = 0;
  uint32_t run = 0;
  State state = State::Requested;
  ReturnMode returnMode = ReturnMode::Restart;
  uint32_t timerArmed = 0;
  uint32_t beforeFree = 0, beforeBlock = 0, afterFree = 0, afterBlock = 0;
  uint32_t frameBytes = 0, released = 0;
  Stats::Record baseline{};
  Stats::Record result{};
  uint32_t lightSleeps = 0, lightSleepMs = 0, standbyMs = 0;
  uint32_t fromWifi = 0;
  Checkpoint checkpoint = Checkpoint::None;
  uint32_t batteryPercent = 0;
  uint32_t check = 0;
};
inline constexpr const char* RESULT_PATH = "/.crosspoint/dev-ble-cycle";
bool read(Record& record);
bool request(uint32_t run, ReturnMode mode = ReturnMode::Restart);
// Arms the saved-network return BEFORE running the experiment. Failure never
// starts a radio experiment; the caller returns to Same Wi-Fi instead.
bool begin();
bool interruptIfRunning();
bool active();
bool timerRequested();
bool standbyRequested();
void timerArmed(bool armed);
void startedOnWifi(bool connected);
uint32_t runNumber();
void standbyEvidence(uint32_t sleeps, uint32_t sleepMs, uint32_t elapsedMs);
void frameReleased(uint32_t beforeFree, uint32_t beforeBlock, uint32_t afterFree, uint32_t afterBlock,
                   uint32_t frameBytes, bool released);
bool checkpoint(Checkpoint stage, uint32_t batteryPercent, const Stats::Record* stats = nullptr);
bool complete(const Stats::Record* stats);
}  // namespace Pocket::NearbySync::DevSleepCycle
#endif
