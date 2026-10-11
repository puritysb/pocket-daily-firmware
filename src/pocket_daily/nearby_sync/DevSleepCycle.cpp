#include "DevSleepCycle.h"

#ifdef ENABLE_DEV_REMOTE_FLASH
#include <HalStorage.h>
#include <Logging.h>

#include <cstddef>
#include <cstring>

#include "ExchangeWindow.h"
#include "pocket_daily/product_identity.h"

namespace Pocket::NearbySync::DevSleepCycle {
namespace {
constexpr uint32_t MAGIC = 0x424c4505;
// Fixed developer evidence only, under 208 B; avoids a heap allocation while
// measuring reclamation. Production builds contain neither record nor routes.
Record current;
bool running = false;
static_assert(sizeof(Record) <= 208);

uint32_t checksum(const Record& record) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(&record);
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < offsetof(Record, check); ++i) hash = (hash ^ bytes[i]) * 16777619u;
  return hash;
}

bool save(Record& record) {
  record.magic = MAGIC;
  record.check = checksum(record);
  {
    HalFile file;
    if (!Storage.openFileForWrite("BLEDEV", RESULT_PATH, file) ||
        file.write(reinterpret_cast<const uint8_t*>(&record), sizeof(record)) != sizeof(record))
      return false;
  }
  Record verified;
  return read(verified) && memcmp(&record, &verified, sizeof(record)) == 0;
}

bool armBoot(const char mode) {
  bool saved = false;
  {
    HalFile file;
    saved = Storage.openFileForWrite("BLEDEV", PocketDaily::DEV_BOOT_FILE_TRANSFER_MARKER, file) &&
            file.write(reinterpret_cast<const uint8_t*>(&mode), 1) == 1;
  }
  {
    HalFile file = Storage.open(PocketDaily::DEV_BOOT_FILE_TRANSFER_MARKER);
    char actual = 0;
    saved = saved && file && file.size() == 1 && file.read(&actual, 1) == 1 && actual == mode;
  }
  if (!saved) Storage.remove(PocketDaily::DEV_BOOT_FILE_TRANSFER_MARKER);
  return saved;
}
}  // namespace

bool read(Record& record) {
  HalFile file = Storage.open(RESULT_PATH);
  if (!file || file.size() != sizeof(record) || file.read(&record, sizeof(record)) != sizeof(record)) return false;
  return record.magic == MAGIC && record.run != 0 && record.check == checksum(record) &&
         (record.returnMode == ReturnMode::Restart || record.returnMode == ReturnMode::TimedDeepSleep ||
          record.returnMode == ReturnMode::AppStandby) &&
         (record.state == State::Requested || record.state == State::Running || record.state == State::Completed ||
          record.state == State::Interrupted);
}

bool request(const uint32_t run, const ReturnMode mode) {
  if (!run || running) return false;
  Record request;
  request.run = run;
  request.returnMode = mode;
  return save(request) && armBoot('B');
}

bool begin() {
  if (running) return false;
  if (!read(current) || current.state != State::Requested || !armBoot('S')) return false;
  current.state = State::Running;
  if (const auto* stats = Window::statistics()) current.baseline = *stats;
  if (!save(current)) return false;
  running = true;
  return true;
}

uint32_t runNumber() { return running ? current.run : 0; }

bool active() { return running; }
bool timerRequested() { return running && current.returnMode == ReturnMode::TimedDeepSleep; }
bool standbyRequested() { return running && current.returnMode == ReturnMode::AppStandby; }
void startedOnWifi(const bool connected) {
  if (running) current.fromWifi = connected;
}
void standbyEvidence(const uint32_t sleeps, const uint32_t sleepMs, const uint32_t elapsedMs) {
  if (!running) return;
  current.lightSleeps = sleeps;
  current.lightSleepMs = sleepMs;
  current.standbyMs = elapsedMs;
}
void timerArmed(const bool armed) {
  if (running) current.timerArmed = armed;
}

bool interruptIfRunning() {
  if (running || !read(current) || current.state != State::Running) return false;
  current.state = State::Interrupted;
  if (!save(current)) LOG_ERR("BLEDEV", "Could not record interrupted cycle");
  return true;
}

void frameReleased(const uint32_t beforeFree, const uint32_t beforeBlock, const uint32_t afterFree,
                   const uint32_t afterBlock, const uint32_t frameBytes, const bool released) {
  if (!running) return;
  current.beforeFree = beforeFree;
  current.beforeBlock = beforeBlock;
  current.afterFree = afterFree;
  current.afterBlock = afterBlock;
  current.frameBytes = frameBytes;
  current.released = released;
}

bool checkpoint(const Checkpoint stage, const uint32_t batteryPercent, const Stats::Record* stats) {
  if (!running) return false;
  current.checkpoint = stage;
  current.batteryPercent = batteryPercent;
  if (stats) current.result = *stats;
  return save(current);
}

bool complete(const Stats::Record* stats) {
  if (!running) return false;
  running = false;
  if (stats) current.result = *stats;
  current.state = State::Completed;
  if (save(current)) return true;
  LOG_ERR("BLEDEV", "Could not verify cycle result");
  return false;
}
}  // namespace Pocket::NearbySync::DevSleepCycle
#endif
