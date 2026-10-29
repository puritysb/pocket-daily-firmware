#pragma once
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#define RTC_NOINIT_ATTR
inline unsigned long millis() { return 123; }
class Print {
 public:
  virtual ~Print() = default;
  virtual size_t write(uint8_t) { return 0; }
  virtual size_t write(const uint8_t*, size_t) { return 0; }
  virtual void flush() {}
};
