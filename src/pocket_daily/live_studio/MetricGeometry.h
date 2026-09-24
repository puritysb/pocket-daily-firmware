#pragma once

#include <cstdint>
#include <limits>

namespace PocketDaily::LiveStudio::MetricGeometry {
// Pagination always needs a nonzero divisor, even if a pack or a very small
// viewport leaves no room for a row. This does not validate every drawing bound.
constexpr int rowStep(int height, int gap = 0) {
  const int64_t step = static_cast<int64_t>(height) + gap;
  return step < 1                                 ? 1
         : step > std::numeric_limits<int>::max() ? std::numeric_limits<int>::max()
                                                  : static_cast<int>(step);
}
constexpr int pageItems(int64_t available, int step) {
  if (available <= 0) return 1;
  const int64_t count = available / rowStep(step);
  return count < 1                                 ? 1
         : count > std::numeric_limits<int>::max() ? std::numeric_limits<int>::max()
                                                   : static_cast<int>(count);
}
}  // namespace PocketDaily::LiveStudio::MetricGeometry
