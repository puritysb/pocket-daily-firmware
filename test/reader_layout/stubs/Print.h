#pragma once
#include <cstddef>
#include <cstdint>
class Print {
 public:
  virtual ~Print() = default;
  virtual size_t write(uint8_t c) = 0;
  virtual size_t write(const uint8_t* buffer, size_t size) {
    size_t n = 0;
    while (n < size && write(buffer[n])) ++n;
    return n;
  }
  size_t write(const char* s, size_t n) { return write(reinterpret_cast<const uint8_t*>(s), n); }
};
