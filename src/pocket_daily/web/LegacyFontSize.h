#pragma once

#include <cstdint>

namespace PocketDaily::Web::LegacyFontSize {
// /api/pocket/v1/preferences keeps the app's 0..3 wire slots even though
// upstream settings now persist physical points. No allocation or extra state.
inline constexpr uint8_t COUNT = 4;
inline constexpr uint8_t POINTS[COUNT] = {12, 14, 16, 18};
constexpr uint8_t toPoints(uint8_t slot) { return POINTS[slot < COUNT ? slot : 1]; }
constexpr uint8_t fromPoints(uint8_t points) {
  // Intermediate/new reader sizes project to the closest legacy choice, ties lower.
  if (points <= 13) return 0;
  if (points <= 15) return 1;
  if (points <= 17) return 2;
  return 3;
}
}  // namespace PocketDaily::Web::LegacyFontSize
