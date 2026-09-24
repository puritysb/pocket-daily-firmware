#pragma once

#include <cstdint>

#include "DevBootReturn.h"

class GfxRenderer;

namespace PocketDaily::Boot {

// Pocket-owned silent-reboot targets appended after upstream's HOME(0) and
// READER(1); main.cpp's routing reads these alongside its own. The RTC_NOINIT
// words stay defined in main.cpp next to upstream's silent-reboot state.
inline constexpr uint32_t kRebootTargetPocketDaily = 2;
inline constexpr uint32_t kRebootTargetPocketNearbySync = 3;
// Widest legal target for setup()'s read-and-clear bound check.
inline constexpr uint32_t kRebootTargetMax = kRebootTargetPocketNearbySync;

// Wire the host-owned globals the boot hooks need: the renderer the restart
// popup and the preview capture draw through, and upstream's deep-sleep latch
// (a committed sleep supersedes any heap-defrag reboot). Call once in setup().
void begin(GfxRenderer& renderer, const bool& deepSleepInProgress);

// Embedded PocketSansJP subset to the SD font store, revision-gated.
void installJapaneseFont();

// Boot-time init split in two to preserve ordering: the net-health ring
// starts before settings load; the persisted UI pack layers over the theme
// only after the theme has reloaded.
void beginNetHealth();
void applyStartupUiPack();

// Dev builds only: read and remove the one-shot mode marker before any radio
// attempt. Invalid/unremovable markers never trigger automatic connection.
DevBootReturn consumeDevBootReturn();

}  // namespace PocketDaily::Boot

// silentRestartToPocketDaily() / silentRestartToPocketNearbySync() stay
// declared by upstream's SilentRestart.h; ProductBoot.cpp defines them so
// main.cpp keeps only upstream code and existing call sites are unchanged.
