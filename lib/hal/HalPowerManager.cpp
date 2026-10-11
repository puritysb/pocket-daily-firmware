#include "HalPowerManager.h"

#include <BoardConfig.h>
#include <Logging.h>
#include <PowerManager.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <soc/soc_caps.h>

#include <cassert>
#ifdef ENABLE_BLE_STANDBY
#include <atomic>
namespace {
DRAM_ATTR std::atomic<uint32_t> standbySleeps{0};
DRAM_ATTR std::atomic<uint32_t> standbySleepMs{0};
bool sleepObserverReady = false;
esp_err_t IRAM_ATTR afterStandbySleep(int64_t actualUs, void*) {
  if (actualUs > 0) {
    standbySleeps.fetch_add(1, std::memory_order_relaxed);
    standbySleepMs.fetch_add(static_cast<uint32_t>(actualUs / 1000), std::memory_order_relaxed);
  }
  return ESP_OK;
}
}  // namespace
#endif

#include "HalGPIO.h"

#if FREEINK_DEVICE_PAPERMONO
#include <M5Pm1.h>
#endif

HalPowerManager powerManager;  // Singleton instance

// GPIO13 controls the X4 battery latch and the X3 SD power rail on the C3
// Xteink boards. Other boards use it for unrelated signals, including the
// X4 Pro display chip select.
static constexpr gpio_num_t XTEINK_C3_GPIO13 = GPIO_NUM_13;

void HalPowerManager::begin() {
  if (BoardConfig::ACTIVE.batteryAdc >= 0) {
    pinMode(BoardConfig::ACTIVE.batteryAdc, INPUT);
  }
  normalFreq = getCpuFrequencyMhz();
  modeMutex = xSemaphoreCreateMutex();
  assert(modeMutex != nullptr);
#ifdef ENABLE_BLE_STANDBY
#if !CONFIG_PM_ENABLE || !CONFIG_FREERTOS_USE_TICKLESS_IDLE || !CONFIG_BT_CTRL_MODEM_SLEEP
#error BLE standby requires the dedicated power-management SDK build
#endif
  esp_pm_config_t config{};
  config.max_freq_mhz = normalFreq;
  config.min_freq_mhz = 40;
  config.light_sleep_enable = false;
  // The SDK owns this small lock allocation for the firmware lifetime. A
  // stack object cannot substitute for its opaque reference-counted handle.
  if (esp_pm_configure(&config) != ESP_OK ||
      esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "reader", &frequencyLock) != ESP_OK) {
    LOG_ERR("PWR", "Could not initialize standby power management");
  }
  // SDK-owned callback registration; no buffer or allocation per sleep/wake.
  esp_pm_sleep_cbs_register_config_t callbacks{};
  callbacks.exit_cb = afterStandbySleep;
  sleepObserverReady = esp_pm_light_sleep_register_cbs(&callbacks) == ESP_OK;
#endif
}

void HalPowerManager::setPowerSaving(bool enabled) {
  if (normalFreq <= 0) {
    return;  // invalid state
  }

  auto wifiMode = WiFi.getMode();
  if (wifiMode != WIFI_MODE_NULL) {
    // Wifi is active, force disabling power saving
    enabled = false;
  }

#ifdef ENABLE_BLE_STANDBY
  if (!frequencyLock) return;
  xSemaphoreTake(modeMutex, portMAX_DELAY);
  const bool needsMaximum = !enabled || currentLockMode != None;
  if (needsMaximum != frequencyLocked) {
    const esp_err_t result = needsMaximum ? esp_pm_lock_acquire(frequencyLock) : esp_pm_lock_release(frequencyLock);
    if (result == ESP_OK) frequencyLocked = needsMaximum;
  }
  xSemaphoreGive(modeMutex);
#else
  // The legacy backend tolerates a stale mode read; the PM lock backend above
  // serializes changes because acquiring/releasing is reference-counted.
  const LockMode mode = currentLockMode;
  if (mode == None && enabled && !isLowPower) {
    LOG_DBG("PWR", "Going to low-power mode");
    if (!setCpuFrequencyMhz(LOW_POWER_FREQ)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", LOW_POWER_FREQ);
      return;
    }
    isLowPower = true;

  } else if ((!enabled || mode != None) && isLowPower) {
    LOG_DBG("PWR", "Restoring normal CPU frequency");
    if (!setCpuFrequencyMhz(normalFreq)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", normalFreq);
      return;
    }
    isLowPower = false;
  }

  // Otherwise, no change needed
#endif
}

void HalPowerManager::startDeepSleep(HalGPIO& gpio) const {
  gpio.detachPowerButtonLatch();
#ifdef ENABLE_SERIAL_LOG
  // Tear down HWCDC so the host sees a clean disconnect and the peripheral
  // doesn't hold power domains that interfere with USB-powered GPIO wake.
  // logSerial is the raw HWCDC reference; Serial is the MySerialImpl proxy
  // (which doesn't expose end()).
  logSerial.end();
#endif

#if !SOC_PM_SUPPORT_EXT1_WAKEUP
  if (gpio.isXteinkDevice()) {
    // GPIO13 is the X4 battery latch and the X3 SD power enable. Driving it
    // low powers off the corresponding rail; only X4 loses battery CPU power.
    // Release any surviving pad hold first: hold_en survives deep sleep via
    // the SDK's deepSleep() (esp_sleep_config_gpio_isolate +
    // gpio_deep_sleep_hold_en), and a held pad silently ignores the drive.
    gpio_hold_dis(XTEINK_C3_GPIO13);
    gpio_set_direction(XTEINK_C3_GPIO13, GPIO_MODE_OUTPUT);
    gpio_set_level(XTEINK_C3_GPIO13, 0);
    gpio_hold_en(XTEINK_C3_GPIO13);
  }
#endif

  // Hold every configured power-latch pin HIGH through deep sleep. These are
  // keep-alive enables (the X4 Pro's master peripheral rail on GPIO1, the
  // Sticky's PWR_HOLD/PWR_LOCK): deepSleep() isolates all pads
  // (esp_sleep_config_gpio_isolate), so a latch without an armed hold loses its
  // output driver and floats — on the X4 Pro the latch drops as soon as
  // external power leaves (serial/pogo adapter unplugged), and the next power-
  // button press cold-boots instead of fast-waking. holdPowerRails() asserted
  // the latches at boot but arms no sleep hold; arm it here instead. Skips
  // XTEINK_C3_GPIO13: it is power.latch0 on X4, where the block above drives
  // it LOW on purpose (battery power-off); X3 declares it as the SD enable.
  for (const int8_t pin : {BoardConfig::ACTIVE.power.latch0, BoardConfig::ACTIVE.power.latch1}) {
    if (pin < 0 || static_cast<gpio_num_t>(pin) == XTEINK_C3_GPIO13) continue;
    const auto g = static_cast<gpio_num_t>(pin);
    // Release any surviving pad hold first: a held pad silently ignores the
    // drive below (same trap as the GPIO13 block above).
    gpio_hold_dis(g);
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
    gpio_hold_en(g);
  }

  // Cut the gated peripheral rails (touch/SD/EPD on boards like the Sticky) and
  // hold the enables off through deep sleep — otherwise the GT911 and SD card
  // stay powered all through "off" and drain the battery. No-op on boards with
  // no switched rails (X4); X3 also cuts its SD rail. Trade-off: no touch-to-wake; wake is the power
  // button. Must run after display.deepSleep() so the panel controller gets its
  // deep-sleep command while its rail is still up (enterDeepSleep() in main.cpp
  // guarantees that ordering).
  freeink::PowerManager::powerDownRailsForSleep();

#if FREEINK_DEVICE_PAPERMONO
  // Its power button is behind the M5PM1 PMIC rather than an ESP GPIO, so
  // normal GPIO deep sleep would have no wake source. Ask the PMIC to shut the
  // device down; a button click then restarts it through a cold boot.
  if (freeink::m5pm1::requestShutdown()) {
    delay(1000);  // allow the PMIC firmware time to drop power
  }
#endif

  // Waits for the power button to be physically released (so holding it doesn't
  // immediately wake the device again), then arms the wake source and sleeps.
  freeink::PowerManager::deepSleepUntilPowerButton();
}

#ifdef ENABLE_DEV_REMOTE_FLASH
bool HalPowerManager::armDevSleepTimer() const {
  // X4 drops its battery latch in startDeepSleep(), so a timer cannot guarantee
  // its return. Do not generalize this experiment to that board.
  return gpio.deviceIsX3() && esp_sleep_enable_timer_wakeup(3000000ULL) == ESP_OK;
}

bool HalPowerManager::devWokeFromTimer() const { return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER; }
#endif

uint16_t HalPowerManager::getBatteryPercentage() const {
  static const BatteryMonitor battery;
  if (BoardConfig::ACTIVE.batteryGauge.gaugeAddr != 0) {
    const unsigned long now = millis();
    if (_batteryLastPollMs != 0 && (now - _batteryLastPollMs) < BATTERY_POLL_MS) {
      return _batteryCachedPercent;
    }

    _batteryLastPollMs = now;
    uint16_t percent = 0;
    if (!battery.readPercentageChecked(percent)) {
      return _batteryCachedPercent;
    }
    _batteryCachedPercent = percent;
    return _batteryCachedPercent;
  }

  // smooth the battery %.
  if (_batteryCachedPercent == 0) {
    _batteryCachedPercent = 10 * battery.readPercentage();
  } else {
    _batteryCachedPercent = (_batteryCachedPercent * 9 + battery.readPercentage() * 10) / 10;
  }
  return _batteryCachedPercent / 10;
}

HalPowerManager::Lock::Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  // Current limitation: only one lock at a time
  if (powerManager.currentLockMode != None) {
    LOG_ERR("PWR", "Lock already held, ignore");
    valid = false;
  } else {
    powerManager.currentLockMode = NormalSpeed;
    valid = true;
  }
  xSemaphoreGive(powerManager.modeMutex);
  if (valid) {
    // Immediately restore normal CPU frequency if currently in low-power mode
    powerManager.setPowerSaving(false);
  }
}

bool HalPowerManager::beginBleStandby() {
#ifdef ENABLE_BLE_STANDBY
  if (!gpio.deviceIsX3() || !frequencyLock || !sleepObserverReady || currentLockMode != None) return false;
  // This board keeps SD mounted and polls its physical inputs during standby.
  // The inherited SDK's PM_SLP_DISABLE_GPIO otherwise switches *every* pin to
  // its disabled sleep configuration, including the SD supply and button pulls.
  // Preserve the already configured levels/functions across automatic light
  // sleep; terminal deep sleep still explicitly isolates/powers down its rails.
  // SDK v5.5.5 components/esp_pm/Kconfig documents this per-pin override.
  for (int pin = 0; pin < SOC_GPIO_PIN_COUNT; ++pin) {
    if (!GPIO_IS_VALID_GPIO(pin)) continue;
    if (gpio_sleep_sel_dis(static_cast<gpio_num_t>(pin)) != ESP_OK) return false;
  }
  standbySleeps.store(0, std::memory_order_relaxed);
  standbySleepMs.store(0, std::memory_order_relaxed);
  esp_pm_config_t config{};
  config.max_freq_mhz = normalFreq;
  config.min_freq_mhz = 40;
  config.light_sleep_enable = true;
  if (esp_pm_configure(&config) != ESP_OK) {
    LOG_ERR("PWR", "Could not enable BLE standby");
    return false;
  }
  setPowerSaving(true);
  return true;
#else
  return false;
#endif
}

uint32_t HalPowerManager::bleStandbySleepCount() const {
#ifdef ENABLE_BLE_STANDBY
  return standbySleeps.load(std::memory_order_relaxed);
#else
  return 0;
#endif
}
uint32_t HalPowerManager::bleStandbySleepMillis() const {
#ifdef ENABLE_BLE_STANDBY
  return standbySleepMs.load(std::memory_order_relaxed);
#else
  return 0;
#endif
}

HalPowerManager::Lock::~Lock() { release(); }

void HalPowerManager::Lock::release() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  if (valid) {
    powerManager.currentLockMode = None;
    valid = false;
  }
  xSemaphoreGive(powerManager.modeMutex);
}
