#pragma once

#ifdef ENABLE_DEV_REMOTE_FLASH
#include <Arduino.h>
namespace PocketDaily::WakeClockTrace {
bool arm();
void beforeClockRestore();
void afterClockRestore();
void autoSleep();
void firstHome(uint32_t date, uint16_t minute);
String report();
}  // namespace PocketDaily::WakeClockTrace
#endif
