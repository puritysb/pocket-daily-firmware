#pragma once
// Host stand-ins for the Arduino surface the reader's layout and page pipeline use.
#include <Logging.h>
#include <WString.h>

#include <cassert>
#include <cstdint>
#include <cstdlib>

struct ReaderLayoutTestEsp {
  [[noreturn]] void restart() const { std::abort(); }
  uint32_t getFreeHeap() const { return 200000; }
  uint32_t getMaxAllocHeap() const { return 100000; }
};
inline ReaderLayoutTestEsp ESP;
inline void delay(unsigned long) {}

inline void vTaskDelay(unsigned long) {}
