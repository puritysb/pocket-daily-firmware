#pragma once

#include <cstdint>

// AgentDeck M6 pull-sync power ladder (implemented in main.cpp).
//
// Timed deep sleep between pull syncs: saves the current frame for a
// quick-resume wake, seeds the wake-instant epoch estimate into RTC memory,
// tears down WiFi, and arms the sleep timer + power-button wake. The battery
// latch stays held so the timer can fire on battery (see
// HalPowerManager::startTimedDeepSleep). Does not return.
//
// Contract: call from the main/loop task with no render in flight
// (requestUpdateAndWait() first) — mirrors the OTA flash path.
// `epochNowSec` = best current unix-seconds estimate, 0 when unknown; it lets
// the wake boot seed the system clock so "as of" ages stay honest before any
// network sync.
void enterTimedDeepSleep(uint32_t seconds, uint32_t epochNowSec);
