#pragma once

#include <cstdint>

// Device side of the staged-firmware prompt (pocket_daily/staged_firmware.h).
// One 16-byte marker in RTC_NOINIT memory: no heap, no SD write, survives the
// session teardown restart and is lost on power-off.
namespace PocketDaily::StagedFirmware {

// Called after the verified Pocket commit published kPublishPath.
void notePublished(uint32_t size, uint32_t crc32);

// Consumed once per boot, before activity routing, whatever the route. True
// when the boot should offer the confirmation for kPublishPath: a silent
// restart landing on the Pocket Daily or Library shell with the published
// file still present at its recorded size.
bool consumeOffer(bool silentRestart, bool landsOnShell);

}  // namespace PocketDaily::StagedFirmware
