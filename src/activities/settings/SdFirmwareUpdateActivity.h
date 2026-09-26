#pragma once

#include <string>

#include "activities/Activity.h"

/**
 * SD-card based firmware update activity.
 *
 * Flow:
 *  1) onEnter -> push FileBrowserActivity in PickFirmware mode (only .bin files visible).
 *  2) On result: validate the .bin (header magic, size fits OTA partition).
 *  3) Push ConfirmationActivity ("Update firmware?").
 *  4) On confirm: stream the file into the OTA partition via the Arduino Update API,
 *     drawing a progress bar; on success ESP.restart().
 *
 * Used both from Settings -> System -> "SD Card Firmware Update", and as the only
 * activity launched in boot recovery mode (left side button + power on X3).
 *
 * Staged mode (presetPath) skips step 1 for a file the companion published in
 * the transfer session that just ended: it validates the preset path, skips
 * silently when the image carries the running firmware's version, and
 * otherwise shows the same confirmation. Cancel keeps the file and returns.
 */
class SdFirmwareUpdateActivity : public Activity {
 public:
  enum class State {
    PICKING,
    VALIDATING,
    CONFIRMING,
    UPDATING,
    SUCCESS,
    FAILED,
  };

  explicit SdFirmwareUpdateActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool recoveryMode = false)
      : Activity("SdFirmwareUpdate", renderer, mappedInput), recoveryMode(recoveryMode) {}
  // Staged mode: confirm this exact file instead of opening the picker.
  SdFirmwareUpdateActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* presetPath)
      : Activity("SdFirmwareUpdate", renderer, mappedInput), presetPath(presetPath) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == State::UPDATING || state == State::VALIDATING; }
  bool skipLoopDelay() override { return state == State::UPDATING; }

 private:
  State state = State::PICKING;
  bool recoveryMode = false;
  // Non-null in staged mode; points at a string literal (static storage).
  const char* presetPath = nullptr;
  // Staged mode runs validation from loop(), after the first paint.
  bool presetValidationPending = false;

  std::string firmwarePath;
  size_t firmwareSize = 0;
  size_t writtenBytes = 0;
  unsigned int lastRenderedPercent = 101;
  std::string errorMessage;
  // Version string read from the image during validation (empty if absent).
  std::string stagedVersion;

  void launchPicker();
  void onPickerResult(const ActivityResult& result);
  void validateAndConfirm();
  bool validateFirmware();
  void promptConfirmation();
  void onConfirmationResult(const ActivityResult& result);
  void performUpdate();
};
