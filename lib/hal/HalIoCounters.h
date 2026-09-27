#pragma once

#include <cstdint>

// Cumulative SD traffic through HalStorage since boot, for timing telemetry
// (src/pocket_daily/ReaderPerf). Incremented under the storage lock; readers
// take unsynchronized snapshots of these aligned 32-bit words and use deltas,
// so wraparound is harmless.
struct HalIoCounters {
  uint32_t opens = 0;      // successful file opens (every open walks the FAT directory path)
  uint32_t readCalls = 0;  // HalFile::read calls
  uint32_t readBytes = 0;  // bytes returned by HalFile::read
};

inline HalIoCounters halIoCounters;
