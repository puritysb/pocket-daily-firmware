#pragma once
#include "MemoryTestState.h"
struct RenderLock {
  RenderLock() {
    WindowMemoryTest::locked = true;
    ++WindowMemoryTest::locks;
  }
  ~RenderLock() { WindowMemoryTest::locked = false; }
};
