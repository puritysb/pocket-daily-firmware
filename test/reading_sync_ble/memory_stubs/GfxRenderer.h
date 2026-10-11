#pragma once
#include "FontCacheManager.h"
#include "MemoryTestState.h"
class GfxRenderer {
 public:
  FontCacheManager* caches = nullptr;
  FontCacheManager* getFontCacheManager() const { return caches; }
  bool frameReleased = false;
  bool releaseFrameBufferForSleep() {
    frameReleased = WindowMemoryTest::locked;
    return frameReleased;
  }
};
