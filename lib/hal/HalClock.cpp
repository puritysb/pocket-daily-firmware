#include "HalClock.h"

#include <Logging.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <time.h>
#if CONFIG_IDF_TARGET_ESP32C3
#include <Preferences.h>
#include <esp_attr.h>
#include <esp_private/esp_clk.h>
#include <esp_system.h>
#include <soc/rtc.h>
#endif

HalClock halClock;  // Singleton instance

#if CONFIG_IDF_TARGET_ESP32C3
namespace {
constexpr uint32_t kDeskClockMagic = 0x50444331;
constexpr uint64_t kMaxElapsedSeconds = 31ULL * 86400;
constexpr const char* kClockNamespace = "pocketclock";
constexpr const char* kOffsetKey = "offset";
struct ClockAnchor {
  uint64_t ticks;
  uint32_t calibration;
  uint32_t epoch;
  uint32_t magic;
};
RTC_NOINIT_ATTR ClockAnchor companionAnchor;
int16_t companionOffset = HalClock::UNSPECIFIED_OFFSET;
portMUX_TYPE companionTimeLock = portMUX_INITIALIZER_UNLOCKED;

bool validOffset(int16_t offset) { return offset >= -720 && offset <= 840; }
}  // namespace
#endif

void HalClock::begin() {
#if CONFIG_IDF_TARGET_ESP32C3
  // Software return from Sync and deep sleep keep the hardware counter.
  // Power loss and other reset causes must not reuse a stale anchor.
  const auto reset = esp_reset_reason();
  if (reset != ESP_RST_SW && reset != ESP_RST_DEEPSLEEP) companionAnchor.magic = 0;
  Preferences prefs;
  if (prefs.begin(kClockNamespace, true)) {
    const int16_t saved = prefs.getShort(kOffsetKey, UNSPECIFIED_OFFSET);
    if (validOffset(saved)) companionOffset = saved;
  }
#endif
  _available = _sdkRtc.begin();
  LOG_INF("CLK", _available ? "SDK RTC found" : "RTC not found");
}

bool HalClock::setCompanionTime(const uint32_t epoch, const int16_t offsetMinutes) {
#if CONFIG_IDF_TARGET_ESP32C3
  if (epoch < COMPANION_MIN_EPOCH || epoch >= COMPANION_MAX_EPOCH || !validOffset(offsetMinutes)) return false;
  const uint32_t calibration = esp_clk_slowclk_cal_get();
  if (!calibration) return false;
  const uint64_t ticks = rtc_time_get();
  // Only the web task writes; readers copy the complete state under the lock.
  if (offsetMinutes != companionOffset) {
    Preferences prefs;
    if (!prefs.begin(kClockNamespace, false) || prefs.putShort(kOffsetKey, offsetMinutes) != sizeof(offsetMinutes))
      return false;
  }
  portENTER_CRITICAL(&companionTimeLock);
  companionAnchor = {ticks, calibration, epoch, kDeskClockMagic};
  companionOffset = offsetMinutes;
  portEXIT_CRITICAL(&companionTimeLock);
  return true;
#else
  (void)epoch;
  (void)offsetMinutes;
  return false;
#endif
}

HalClock::CompanionTime HalClock::companionTime(const time_t systemTime) const {
#if CONFIG_IDF_TARGET_ESP32C3
  portENTER_CRITICAL(&companionTimeLock);
  const auto anchor = companionAnchor;
  const auto offset = companionOffset;
  portEXIT_CRITICAL(&companionTimeLock);
  if (anchor.magic == kDeskClockMagic && anchor.calibration && anchor.epoch >= COMPANION_MIN_EPOCH &&
      anchor.epoch < COMPANION_MAX_EPOCH && validOffset(offset)) {
    const uint64_t ticks = rtc_time_get();
    if (ticks >= anchor.ticks && ticks - anchor.ticks <= UINT64_MAX / anchor.calibration) {
      const uint64_t seconds = rtc_time_slowclk_to_us(ticks - anchor.ticks, anchor.calibration) / 1000000;
      if (seconds <= kMaxElapsedSeconds && anchor.epoch + seconds < COMPANION_MAX_EPOCH)
        return {static_cast<time_t>(anchor.epoch + seconds), offset};
    }
  }
  return {systemTime, offset};
#else
  return {systemTime, UNSPECIFIED_OFFSET};
#endif
}

namespace {
// UTC calendar date -> Unix epoch, no timezone involvement (newlib has no
// timegm). Days-from-civil per Howard Hinnant's algorithm.
time_t epochFromUtc(const Rtc::DateTime& dt) {
  int y = dt.year;
  const int m = dt.month;
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153u * static_cast<unsigned>(m + (m > 2 ? -3 : 9)) + 2u) / 5u + dt.day - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  const long days = static_cast<long>(era) * 146097L + static_cast<long>(doe) - 719468L;
  return static_cast<time_t>(days) * 86400 + dt.hour * 3600L + dt.minute * 60L + dt.second;
}
}  // namespace

void HalClock::setTimezone(const char* posixTz) {
  setenv("TZ", posixTz && posixTz[0] != '\0' ? posixTz : "UTC0", 1);
  tzset();
  _lastPollMs = 0;  // re-derive local time under the new rule immediately
}

bool HalClock::localTime(struct tm& out) const {
  if (!_available) return false;

  const unsigned long now = millis();
  if (_lastPollMs == 0 || (now - _lastPollMs) >= CLOCK_POLL_MS) {
    Rtc::DateTime dt;
    if (_sdkRtc.now(dt)) {
      _cachedUtc = epochFromUtc(dt);
      _hasCachedTime = true;
    } else if (!_hasCachedTime) {
      return false;
    }
    _lastPollMs = now != 0 ? now : 1;  // 0 doubles as the invalidation sentinel
  }
  localtime_r(&_cachedUtc, &out);
  return true;
}

bool HalClock::getTime(uint8_t& hour, uint8_t& minute) const {
  struct tm local;
  if (!localTime(local)) return false;
  hour = static_cast<uint8_t>(local.tm_hour);
  minute = static_cast<uint8_t>(local.tm_min);
  return true;
}

bool HalClock::formatTime(char* buf, size_t bufSize, bool use12Hour) const {
  if (bufSize < (use12Hour ? 9u : 6u)) return false;
  struct tm local;
  if (!localTime(local)) return false;

  if (use12Hour) {
    const bool pm = local.tm_hour >= 12;
    int hour12 = local.tm_hour % 12;
    if (hour12 == 0) hour12 = 12;
    snprintf(buf, bufSize, "%d:%02d %s", hour12, local.tm_min, pm ? "PM" : "AM");
  } else {
    snprintf(buf, bufSize, "%02d:%02d", local.tm_hour, local.tm_min);
  }
  return true;
}

bool HalClock::syncFromNTP() {
  if (!_available) return false;

  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("CLK", "WiFi not connected, cannot sync NTP");
    return false;
  }

  LOG_INF("CLK", "Starting NTP sync...");
  // configTzTime overwrites the process TZ with UTC0 for the SNTP exchange;
  // remember the display timezone so it can be restored below.
  const char* tzBefore = getenv("TZ");
  char savedTz[64] = {0};
  if (tzBefore) snprintf(savedTz, sizeof(savedTz), "%s", tzBefore);
  configTzTime("UTC0", "pool.ntp.org", "time.nist.gov");

  // Wait for SNTP sync to complete (up to 5 seconds)
  constexpr int maxAttempts = 50;
  for (int i = 0; i < maxAttempts; i++) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      time_t now = time(nullptr);
      struct tm timeinfo;
      gmtime_r(&now, &timeinfo);

      Rtc::DateTime dt;
      dt.year = static_cast<uint16_t>(timeinfo.tm_year + 1900);
      dt.month = static_cast<uint8_t>(timeinfo.tm_mon + 1);
      dt.day = static_cast<uint8_t>(timeinfo.tm_mday);
      dt.hour = static_cast<uint8_t>(timeinfo.tm_hour);
      dt.minute = static_cast<uint8_t>(timeinfo.tm_min);
      dt.second = static_cast<uint8_t>(timeinfo.tm_sec);
      dt.weekday = static_cast<uint8_t>(timeinfo.tm_wday);
      const bool ok = _sdkRtc.set(dt);
      if (ok) {
        _cachedUtc = epochFromUtc(dt);
        _hasCachedTime = true;
        _lastPollMs = 0;
        LOG_INF("CLK", "RTC set to %04u-%02u-%02u %02u:%02u:%02u UTC", dt.year, dt.month, dt.day, dt.hour, dt.minute,
                dt.second);
      }
      setTimezone(savedTz);
      return ok;
    }
    delay(100);
  }

  LOG_ERR("CLK", "NTP sync timed out");
  setTimezone(savedTz);
  return false;
}
