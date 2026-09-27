#pragma once
// Host stand-ins for the Arduino clock and heap probes ReaderPerf reads.
#include <cstdint>

namespace ReaderPerfTestClock {
inline uint32_t nowMs = 0;
inline uint32_t freeHeap = 100000;
inline uint32_t largestBlock = 60000;
}  // namespace ReaderPerfTestClock

inline unsigned long millis() { return ReaderPerfTestClock::nowMs; }

struct ReaderPerfTestEsp {
  uint32_t getFreeHeap() const { return ReaderPerfTestClock::freeHeap; }
  uint32_t getMaxAllocHeap() const { return ReaderPerfTestClock::largestBlock; }
};
inline ReaderPerfTestEsp ESP;
