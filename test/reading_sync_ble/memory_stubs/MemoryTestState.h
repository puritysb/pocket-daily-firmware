#pragma once
#include <cstdint>
namespace WindowMemoryTest {
inline bool locked = false;
inline bool releasedUnderLock = false;
inline unsigned releases = 0;
inline unsigned locks = 0;
inline uint32_t freeHeap = 0;
inline uint32_t block = 0;
inline uint32_t releasedFree = 0;
inline uint32_t releasedBlock = 0;
}  // namespace WindowMemoryTest
