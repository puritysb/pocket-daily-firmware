#pragma once
#include <string_view>

#include "TrialReaderPolicy.h"
#include "TrialWakePolicy.h"
class EpubReaderActivity;
namespace PocketDaily::TrialReader {
// Inactive without a developer-provisioned, run-bound lease and verified fixture.
// No production HTTP control surface. Normal reader/history/progress paths run.
bool begin();
void loop();
void observe(EpubReaderActivity& reader, std::string_view path);
void loadFailed(std::string_view path);
bool read(Record& record);
bool readWake(TrialWake::Record& record);
bool beginTimerResume(uint32_t run);
bool resuming();
bool preparingAutoWake();
void observeAutoSleep(uint32_t timeoutMs, uint32_t idleMs);
void commitSleep();
#ifdef ENABLE_RTC_PROBE_ARENA
// Explicit diagnostic images only. Let the normal PowerButton boot route run.
bool beginGPIOResume(uint32_t run);
void releaseGPIOProbe(bool nativePowerReason, bool holdVerified);
#endif
#ifdef ENABLE_DEV_REMOTE_FLASH
bool prepare(uint32_t run, uint32_t pages, uint32_t bytes, const char* sha, bool autoWake = false);
bool clear(uint32_t run);
#endif
}  // namespace PocketDaily::TrialReader
