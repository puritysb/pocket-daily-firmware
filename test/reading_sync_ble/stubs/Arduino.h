#pragma once
// Host stand-ins for the clock and heap probes the Reading Sync session reads.
#include <cstdint>

namespace ReadingSyncTestHost {
inline uint32_t nowMs = 0;
inline uint32_t freeHeap = 100000;
inline uint32_t largestBlock = 60000;
}  // namespace ReadingSyncTestHost

inline unsigned long millis() { return ReadingSyncTestHost::nowMs; }

struct ReadingSyncTestEsp {
  uint32_t getFreeHeap() const { return ReadingSyncTestHost::freeHeap; }
  uint32_t getMaxAllocHeap() const { return ReadingSyncTestHost::largestBlock; }
};
inline ReadingSyncTestEsp ESP;
