#pragma once
#include <cstdint>

// 8.8 fixed-point font scaling. Glyph bytes stay on SD/in their existing cache;
// the rasterizer samples them directly into the single framebuffer.
namespace fontScale {
constexpr int metric(int value, uint16_t scale) {
  return (value * static_cast<int>(scale) + (value < 0 ? -128 : 128)) / 256;
}
constexpr int extent(int value, uint16_t scale) { return (value * static_cast<int>(scale) + 255) / 256; }
constexpr uint16_t ratio(uint8_t requested, uint8_t available) {
  return available ? static_cast<uint16_t>((requested * 256u + available / 2u) / available) : 256;
}
}  // namespace fontScale
