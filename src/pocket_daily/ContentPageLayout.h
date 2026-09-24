#pragma once

#include <algorithm>
#include <cstdint>

namespace PocketDaily::Content {
// Shared, allocation-free geometry for device rendering and future host preview.
// Rectangles are layout/line boxes, not a claim about a font's ink bearings.
struct ContentPageLayout {
  int x = 0, y = 0, width = 0, footer = 0, advance = 0, gap = 0;
  bool valid = false;

  unsigned remainingLines(int cursor, unsigned maximum) const {
    if (!valid || cursor < y || cursor >= footer) return 0;
    return std::min(maximum, static_cast<unsigned>((footer - cursor) / advance));
  }
  int imageHeight(int cursor) const {
    if (!valid || cursor < y || cursor >= footer) return 0;
    return std::max(0, footer - cursor - gap);
  }
};

inline ContentPageLayout contentPageLayout(int screenWidth, int screenHeight, int top, int right, int bottom, int left,
                                           int sidePadding, int topPadding, int spacing, int lineHeight) {
  ContentPageLayout out;
  if (screenWidth <= 0 || screenHeight <= 0 || top < 0 || right < 0 || bottom < 0 || left < 0 || lineHeight <= 0)
    return out;
  // Theme integers can span int32. Widen before arithmetic, then narrow only
  // after every coordinate is proven to lie inside the positive int viewport.
  const int64_t padding = std::max(0, sidePadding);
  const int64_t gap = std::max(0, spacing);
  const int64_t x = static_cast<int64_t>(left) + padding;
  const int64_t y = static_cast<int64_t>(top) + std::max(0, topPadding);
  const int64_t width = static_cast<int64_t>(screenWidth) - right - x - padding;
  const int64_t advance = static_cast<int64_t>(lineHeight) + gap;
  const int64_t footer = static_cast<int64_t>(screenHeight) - bottom - advance;
  // Reserve two title and two empty-state lines, plus the title/body gap.
  // The footer owns one independent line box and cannot overlap the body.
  if (width < 4 * advance || footer - y < 4 * advance + gap) return out;
  out.x = static_cast<int>(x);
  out.y = static_cast<int>(y);
  out.width = static_cast<int>(width);
  out.footer = static_cast<int>(footer);
  out.advance = static_cast<int>(advance);
  out.gap = static_cast<int>(gap);
  out.valid = true;
  return out;
}
}  // namespace PocketDaily::Content
