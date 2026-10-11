#pragma once
#include "MemoryTestState.h"
struct FontCacheManager {
  void releaseSdFontCaches() {
    using namespace WindowMemoryTest;
    ++releases;
    releasedUnderLock = locked;
    freeHeap = releasedFree;
    block = releasedBlock;
  }
};
