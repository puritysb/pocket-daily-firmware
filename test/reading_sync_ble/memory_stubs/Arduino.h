#pragma once
#include "MemoryTestState.h"
struct MemoryTestEsp {
  uint32_t getFreeHeap() const { return WindowMemoryTest::freeHeap; }
  uint32_t getMaxAllocHeap() const { return WindowMemoryTest::block; }
};
inline MemoryTestEsp ESP;
