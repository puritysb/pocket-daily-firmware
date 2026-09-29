#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

// Preserve the product's seven-byte prefix. Upstream's ten-byte variant places
// its visible offset where our whole-book percentage lives, so never write it.
namespace ReaderProgressCodec {
constexpr size_t EXTENDED_SIZE = 12;
constexpr uint8_t EXTENSION_TAG = 0xD1;
struct Position {
  uint16_t spine = 0;
  uint16_t page = 0;
  uint16_t pageCount = 0;
  int bookPercent = -1;
  std::optional<uint32_t> visibleOffset;
};
inline bool decode(const uint8_t* data, size_t size, Position& out) {
  if (!data || (size != 4 && size != 6 && size != 7 && size != 10 && size != EXTENDED_SIZE)) return false;
  if (size == EXTENDED_SIZE && data[7] != EXTENSION_TAG) return false;
  Position decoded;
  decoded.spine = data[0] | (static_cast<uint16_t>(data[1]) << 8);
  decoded.page = data[2] | (static_cast<uint16_t>(data[3]) << 8);
  if (decoded.page == UINT16_MAX) decoded.page = 0;
  if (size >= 6) decoded.pageCount = data[4] | (static_cast<uint16_t>(data[5]) << 8);
  if ((size == 7 || size == EXTENDED_SIZE) && data[6] <= 100) decoded.bookPercent = data[6];
  if (size == 10 || size == EXTENDED_SIZE) {
    const size_t start = size == 10 ? 6 : 8;
    uint32_t offset = 0;
    for (size_t i = 0; i < 4; ++i) offset |= static_cast<uint32_t>(data[start + i]) << (8 * i);
    decoded.visibleOffset = offset;
  }
  out = decoded;
  return true;
}
inline size_t encode(const Position& pos, uint8_t (&data)[EXTENDED_SIZE]) {
  data[0] = pos.spine & 0xFF;
  data[1] = pos.spine >> 8;
  data[2] = pos.page & 0xFF;
  data[3] = pos.page >> 8;
  data[4] = pos.pageCount & 0xFF;
  data[5] = pos.pageCount >> 8;
  data[6] = pos.bookPercent >= 0 && pos.bookPercent <= 100 ? static_cast<uint8_t>(pos.bookPercent) : 0xFF;
  if (!pos.visibleOffset) return 7;
  data[7] = EXTENSION_TAG;
  for (size_t i = 0; i < 4; ++i) data[8 + i] = (*pos.visibleOffset >> (8 * i)) & 0xFF;
  return EXTENDED_SIZE;
}
}  // namespace ReaderProgressCodec
