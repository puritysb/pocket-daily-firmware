#pragma once

#ifdef ENABLE_DEV_REMOTE_FLASH
#include <Arduino.h>
namespace PocketDaily::WakeClockTrace {
bool arm();
void beforeClockRestore(bool returningToHome);
void afterClockRestore();
void autoSleep();
void firstHome(uint32_t date, uint16_t minute);
void afterAutoWifi();
String report(bool includeAutoWifi);
}  // namespace PocketDaily::WakeClockTrace
#endif
