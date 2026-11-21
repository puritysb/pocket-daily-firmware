#pragma once

#ifdef ENABLE_DEV_REMOTE_FLASH
#include <stdint.h>

namespace DevButtonTrace {
enum Surface : uint8_t { Other, Home, Reader };
enum Action : uint8_t {
  Sample,
  Screen,
  HomePrev,
  HomeNext,
  HomeSelect,
  ReaderPrev,
  ReaderNext,
  QueuedPrev,
  QueuedNext,
  PagePainted,
  BootStarted,
  BootLogoPainted,
  HomePowerRestoreFailed,
  HomeFrameRestoreFailed,
  HomeWifiRestart,
  SleepStandbyRestart,
  BleStartTimeout
};
struct Event {
  uint32_t ms;
  uint8_t surface, action, raw, held;
};
constexpr unsigned capacity = 160;
void start(bool acrossBoots = false);
void bootStarted(uint8_t reason);
void checkpoint(Action value);
void stop();
void clear();
bool active();
void setSurface(Surface value);
void sample(uint8_t raw, uint8_t held);
void action(Action value);
unsigned count();
bool full();
uint32_t maxGap(Surface value);
Event event(unsigned index);
}  // namespace DevButtonTrace
#endif
