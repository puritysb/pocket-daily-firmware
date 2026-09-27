#pragma once
// Host stand-in for Arduino's Print sink used by the EPUB streamers.
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
};
