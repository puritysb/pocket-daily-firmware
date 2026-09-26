#pragma once

#include "activities/Activity.h"
#include "network/OtaUpdater.h"

class OtaUpdateActivity : public Activity {
 public:
  // Settings: a sub-activity; leaving pops back (Wi-Fi exits via silentRestart).
  // PocketDaily: launched from the Pocket Sync menu; every non-install exit
  // turns Wi-Fi off and silent-restarts into Pocket Daily, which never holds
  // the radio itself.
  enum class Origin : uint8_t { Settings, PocketDaily };

 private:
  enum State {
    WIFI_SELECTION,
    CHECKING_FOR_UPDATE,
    WAITING_CONFIRMATION,
    UPDATE_IN_PROGRESS,
    NO_UPDATE,
    FAILED,
    FINISHED,
    SHUTTING_DOWN
  };

  // Can't initialize this to 0 or the first render doesn't happen
  static constexpr unsigned int UNINITIALIZED_PERCENTAGE = 111;

  State state = WIFI_SELECTION;
  unsigned int lastUpdaterPercentage = UNINITIALIZED_PERCENTAGE;
  OtaUpdater updater;

  Origin origin = Origin::Settings;

  // Heap when the check starts and a one-line reason under "Update failed"
  // (stage, error codes, heap in KB); users have no serial log to read.
  uint32_t startFreeHeap = 0;
  uint32_t startLargestBlock = 0;
  char failureDetail[96] = {};
  // OtaFailure::Kind of the last failure; drives the plain-language reason.
  uint8_t failureKind = 0;

  void onWifiSelectionComplete(bool success);
  void noteFailure(int updaterError, bool installing);
  // Cancel, failure and "no update" exits, per Origin.
  void leave();

 public:
  explicit OtaUpdateActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Origin origin = Origin::Settings)
      : Activity("OtaUpdate", renderer, mappedInput), updater(), origin(origin) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CHECKING_FOR_UPDATE || state == UPDATE_IN_PROGRESS; }
  bool skipLoopDelay() override { return true; }  // Prevent power-saving mode
};
