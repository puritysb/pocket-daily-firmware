#pragma once

#include <Arduino.h>
#include <BatteryMonitor.h>
#include <InputManager.h>
#include <Logging.h>
#include <freertos/semphr.h>

#include <atomic>
#include <cassert>

#include "HalGPIO.h"
#ifdef ENABLE_BLE_STANDBY
#include <esp_pm.h>
#endif

class HalPowerManager;
extern HalPowerManager powerManager;  // Singleton

class HalPowerManager {
  int normalFreq = 0;  // MHz
  bool isLowPower = false;
  std::atomic<bool> bleClockRequired{false};

  mutable int _batteryCachedPercent = 0;         // Last read battery percentage (0-100)
  mutable unsigned long _batteryLastPollMs = 0;  // Timestamp of last battery read in milliseconds

  enum LockMode { None, NormalSpeed };
  LockMode currentLockMode = None;
  SemaphoreHandle_t modeMutex = nullptr;  // Protect access to currentLockMode
#ifdef ENABLE_BLE_STANDBY
  esp_pm_lock_handle_t frequencyLock = nullptr;
  bool frequencyLocked = false;
#endif

 public:
#if BOARD_HAS_PSRAM
  static constexpr int LOW_POWER_FREQ = 80;  // MHz
#else
  static constexpr int LOW_POWER_FREQ = 10;  // MHz
#endif
  static constexpr unsigned long IDLE_POWER_SAVING_MS = 3000;  // ms
  static constexpr unsigned long BATTERY_POLL_MS = 1500;       // ms

  void begin();

  // Control CPU frequency for power saving
  void setPowerSaving(bool enabled);
  // Retain an 80 MHz APB-safe floor while a foreground BLE session owns radio.
  // The existing PM-lock backend conservatively retains its maximum lock.
  void setBleClockRequired(bool required) { bleClockRequired.store(required, std::memory_order_release); }

  // Setup wake up GPIO and enter deep sleep
  // Should be called inside main loop() to handle the currentLockMode
  // wakeOnHeldButton: arm the wake without waiting for the power button's
  // release, so a press that is still down wakes the device straight back up
  // through the normal power-button wake path.
  void startDeepSleep(HalGPIO& gpio, bool wakeOnHeldButton = false) const;
  // Explicit maintenance only: X3 or externally powered C3 X4, 1..180s.
  bool armRecoveryTimer(uint32_t seconds) const;
  // Enables driver-managed light sleep only while the display is quiescent.
  // False on ordinary builds, unsupported boards or PM initialization failure.
  bool bleStandbySupported() const;
  bool beginBleStandby();
  // Resume ordinary UI with automatic light sleep disabled before allocating
  // the framebuffer or dispatching physical input.
  bool endBleStandby();
  uint32_t bleStandbySleepCount() const;
  uint32_t bleStandbySleepMillis() const;
#ifdef ENABLE_DEV_REMOTE_FLASH
  // X3 or externally USB-powered X4: retain the normal deep-sleep path, add a timer so
  // the reader can rejoin saved Wi-Fi without a physical button press.
  bool devSleepTimerSupported() const;
  bool armDevSleepTimer() const;
  bool devWokeFromTimer() const;
#endif

  // Get battery percentage (range 0-100)
  uint16_t getBatteryPercentage() const;

  // RAII helper class to manage power saving locks
  // Usage: create an instance of Lock in a scope to disable power saving, for example when running a task that needs
  // full performance. When the Lock instance is destroyed (goes out of scope), power saving will be re-enabled.
  class Lock {
    friend class HalPowerManager;
    bool valid = false;

   public:
    explicit Lock();
    ~Lock();
    void release();

    // Non-copyable and non-movable
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    Lock(Lock&&) = delete;
    Lock& operator=(Lock&&) = delete;
  };
};
