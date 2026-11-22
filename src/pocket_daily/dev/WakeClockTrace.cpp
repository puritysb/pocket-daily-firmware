#include "WakeClockTrace.h"
#ifdef ENABLE_DEV_REMOTE_FLASH
#include <HalClock.h>
#include <HalStorage.h>
#include <esp_sleep.h>
#include <esp_system.h>

namespace PocketDaily::WakeClockTrace {
namespace {
constexpr char path[] = "/.crosspoint/dev-wake-clock.bin";
struct Sample {
  uint32_t system, desk, rtc, uptime, date;
  int16_t offset;
  uint16_t minute;
  uint32_t reset, wake;
};
int stage = -1;
void capture(int expected, uint32_t date = 0, uint16_t minute = 1440) {
  if (stage != expected) return;
  auto file = Storage.open(path, O_WRONLY | O_APPEND);
  if (!file || file.size() != expected * sizeof(Sample)) return;
  auto now = time(nullptr);
  auto desk = halClock.companionTime(now);
  tm local{};
  auto rtc = halClock.localTime(local) ? mktime(&local) : 0;
  Sample sample{uint32_t(now),
                uint32_t(desk.epoch),
                uint32_t(rtc),
                uint32_t(millis()),
                date,
                desk.offsetMinutes,
                minute,
                uint32_t(esp_reset_reason()),
                uint32_t(esp_sleep_get_wakeup_cause())};
  if (file.write(reinterpret_cast<const uint8_t*>(&sample), sizeof(sample)) == sizeof(sample)) ++stage;
}
}  // namespace
bool arm() {
  HalFile file;
  if (!Storage.openFileForWrite("CLOCK", path, file)) return false;
  stage = 0;
  return true;
}
void beforeClockRestore(bool returningToHome) {
  auto file = Storage.open(path);
  if (!file) return;
  auto size = file.size();
  file.close();
  if (size == 0) stage = 0;
  if (size == sizeof(Sample)) stage = 1;
  if (returningToHome && size == 5 * sizeof(Sample)) stage = 5;
  capture(1);
}
void afterClockRestore() { capture(2); }
void autoSleep() { capture(stage == 6 ? 6 : 0); }
void firstHome(uint32_t date, uint16_t minute) { capture(stage == 5 ? 5 : 3, date, minute); }
void afterAutoWifi() { capture(4); }
String report(bool includeAutoWifi) {
  auto file = Storage.open(path);
  String result = "system desk rtc uptime date offset minute reset wake\n";
  if (!file) return result;
  for (unsigned i = 0; i < (includeAutoWifi ? 7u : 4u); ++i) {
    Sample s{};
    if (file.read(reinterpret_cast<uint8_t*>(&s), sizeof(s)) != sizeof(s)) break;
    char line[134];
    snprintf(line, sizeof(line), "%lu %lu %lu %lu %lu %d %u %lu %lu\n", (unsigned long)s.system, (unsigned long)s.desk,
             (unsigned long)s.rtc, (unsigned long)s.uptime, (unsigned long)s.date, s.offset, s.minute,
             (unsigned long)s.reset, (unsigned long)s.wake);
    result += line;
  }
  return result;
}
}  // namespace PocketDaily::WakeClockTrace
#endif
