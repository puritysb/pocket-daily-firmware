#include "DevButtonTrace.h"

#ifdef ENABLE_DEV_REMOTE_FLASH
#include <Arduino.h>
#include <esp_attr.h>

namespace DevButtonTrace {
namespace {
// Retain completed traces across the X3 restart used to enter Same Wi-Fi.
constexpr uint32_t armed = 0x54334231, stopped = 0x54334232, bootTrace = 0x54334233;
struct Record {
  uint32_t magic, maxGap[3];
  unsigned used;
  Event events[capacity];
};
RTC_NOINIT_ATTR Record record;
Surface surface = Other;
uint32_t lastPoll, began;
uint8_t lastRaw, lastHeld;
bool firstPoll = true;
bool valid() { return (record.magic == armed || record.magic == bootTrace || record.magic == stopped) && record.used <= capacity; }
void append(Action value) {
  if (!active() || (surface == Other && record.magic != bootTrace)) return;
  record.events[record.used++] = {static_cast<uint32_t>(millis()), surface, value, lastRaw, lastHeld};
  if (record.used == capacity) stop();
}
}  // namespace
void start(bool acrossBoots) {
  clear();
  record.magic = acrossBoots ? bootTrace : armed;
}
void clear() {
  record = {};
  surface = Other;
  lastRaw = lastHeld = 0;
  firstPoll = true;
}
void stop() {
  if (valid()) record.magic = stopped;
}
bool active() { return (record.magic == armed || record.magic == bootTrace) && record.used < capacity; }
unsigned count() { return valid() ? record.used : 0; }
bool full() { return count() == capacity; }
uint32_t maxGap(Surface value) { return valid() ? record.maxGap[value] : 0; }
Event event(unsigned index) { return index < count() ? record.events[index] : Event{}; }
void setSurface(Surface value) {
  if (!active()) return;
  if (firstPoll) {
    if (record.used && record.magic != bootTrace) {
      stop();  // A reboot after recording began closes the run.
      return;
    }
    firstPoll = false;
    began = millis();
  }
  if (millis() - began >= 480000) stop();
  if (value == surface) return;
  surface = value;
  lastPoll = millis();
  append(Screen);
}
void sample(uint8_t raw, uint8_t held) {
  if (!active() || surface == Other) return;
  const uint32_t now = millis();
  if (now - began >= 480000) {
    stop();
    return;
  }
  const uint32_t gap = now - lastPoll;
  if (gap > record.maxGap[surface]) record.maxGap[surface] = gap;
  lastPoll = now;
  if (record.magic == bootTrace) {
    constexpr uint8_t confirm = 1u << 1;
    raw &= confirm;
    held &= confirm;
  }
  if (raw == lastRaw && held == lastHeld) return;
  lastRaw = raw;
  lastHeld = held;
  append(Sample);
}
void checkpoint(Action value) {
  if (record.magic == bootTrace) append(value);
}
void bootStarted(uint8_t reason) {
  if (record.magic != bootTrace) return;
  lastRaw = reason;  // BootStarted stores esp_reset_reason() in the raw column.
  checkpoint(BootStarted);
  lastRaw = 0;
}
void action(Action value) { append(value); }
}  // namespace DevButtonTrace
#endif
