#pragma once

#include <cstdint>

#include "ExchangeWindowPolicy.h"
#include "ReadSyncStats.h"

class GfxRenderer;

// Owner of Pocket Reading Sync exchange windows (docs/reading-sync-ble-v1.md):
// applies ExchangeWindowPolicy to NimBLE. Everything is called from the
// Arduino loop task (main loop, ActivityManager transitions, the sleep path).
// Nothing is resident while no window is up except this module's statics
// (a few dozen bytes); the service, session and exchange buffers are
// allocated per window.
namespace Pocket::NearbySync::Window {

// A trigger seen on the shell (book closed, wake). `renderCount` is the
// render task's completed-frame counter, so the window waits for the shell's
// next frame.
void arm(Trigger trigger, uint32_t renderCount);
// One main-loop pass. `shellActive`: the current screen tolerates the radio.
void loop(bool shellActive, uint32_t renderCount, const GfxRenderer& renderer);
// Closes at once and releases NimBLE before returning (a book opening, a
// Wi-Fi mode or the Nearby Sync screen, any other screen).
void close(CloseReason reason);
// NimBLE is starting or up: the loop must keep the CPU at normal speed.
bool active();
// Heap and outcome figures of recent windows (kept across restarts), or null.
const Stats::Record* statistics();
// The 20 s window after the sleep screen is drawn; returns when it closed
// (time up, a button press, or a gate refused it). Extends an open window.
void runBeforeDeepSleep(uint32_t renderCount, const GfxRenderer& renderer);

enum class StandbyExit : uint8_t { DeepSleep, Reader, SameWifi };
bool standbyEligible();
// Framebuffer and panel must already be released/asleep. maxMs is only a
// developer recovery deadline; zero means normal app-requested standby.
StandbyExit runStandby(const GfxRenderer& renderer, uint32_t maxMs = 0);

}  // namespace Pocket::NearbySync::Window
