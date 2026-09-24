#pragma once
#include <cstddef>
#include <cstdint>

namespace PocketDaily::Content {
inline uint32_t contentCrcUpdate(uint32_t crc, const uint8_t* data, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320u : 0u);
  }
  return crc;
}
}  // namespace PocketDaily::Content
