#pragma once
#include <chrono>
#include <cstdint>
#define LOG_ERR(...) \
  do {               \
  } while (0)
#define LOG_INF(...) \
  do {               \
  } while (0)
#define LOG_DBG(...) \
  do {               \
  } while (0)
inline unsigned long micros() {
  return static_cast<unsigned long>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}
inline unsigned long millis() { return micros() / 1000; }
