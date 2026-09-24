#pragma once
// Never writes private content to host logs.
#include <chrono>
#include <cstdint>
template <typename... Args>
inline void hostRenderLog(const char*, Args&&...) {}
#define LOG_ERR(...) hostRenderLog(__VA_ARGS__)
#define LOG_DBG(...) hostRenderLog(__VA_ARGS__)
inline unsigned long micros() {
  return static_cast<unsigned long>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}
inline unsigned long millis() { return micros() / 1000; }
