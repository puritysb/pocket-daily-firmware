#pragma once

#include "TrialLeasePolicy.h"

namespace PocketDaily::TrialLease {
// Always compiled so a real production image can return to its preserved dev
// slot. Only developer firmware can provision the checked NVS lease. Ordinary
// production boots have no lease, no timer and no remote trial/flash endpoint.
void onBoot();
void beforeSleep();
void returnNow();
bool wantsSync();
Surface requestedSurface();
bool recordSurfaceMemory(uint32_t free, uint32_t largest);
void recordHttpMemory(bool ready, uint32_t free, uint32_t largest);
bool active();
const Record* runningTrial();
bool read(Record& record);
#ifdef ENABLE_DEV_REMOTE_FLASH
bool prepare(uint32_t run, uint32_t seconds, const char* imageSHA256, const char* returnSHA256,
             Surface surface = Surface::Sync);
bool verifyDestination();
bool cancelPrepared(uint32_t run);
bool clear(uint32_t run);
#endif
}  // namespace PocketDaily::TrialLease
