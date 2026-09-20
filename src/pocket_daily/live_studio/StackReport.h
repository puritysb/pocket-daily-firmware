#pragma once

#include <cstddef>

// HN-2 evidence part 2: per-task stack high-water marks via
// vTaskGetStackHighWaterMark (does NOT suspend tasks, unlike
// uxTaskGetSystemState which hung the loaded X3). Reported for the tasks we
// own handles/names for, plus the calling task.
namespace PocketDaily::StackReport {

// Appends "task <name> stackFree=<n>" lines into `out`; returns bytes used.
size_t render(char* out, size_t cap);

}  // namespace PocketDaily::StackReport
