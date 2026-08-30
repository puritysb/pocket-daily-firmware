#pragma once

#include <cstdint>
#include <string>

namespace HalSystem {
struct StackFrame {
  uint32_t sp;
  uint32_t spp[8];
};

void begin();

// Dump panic info to SD card if necessary
void checkPanic();
void clearPanic();
// Store one allocation-free runtime checkpoint in RTC memory. It survives a
// panic reboot and is included in crash_report.txt even when normal logging
// could not allocate or flush its final line.
void setCrashBreadcrumb(const char* value);

std::string getPanicInfo(bool full = false);
bool isRebootFromPanic();
// Covers panic/lockup plus watchdog, brownout and power-glitch resets so
// failures that bypass panic_abort still produce a retained report.
bool isRebootFromCrash();
}  // namespace HalSystem
