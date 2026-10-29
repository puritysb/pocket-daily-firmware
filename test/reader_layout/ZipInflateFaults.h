#pragma once

#include <cstddef>

namespace ZipInflateFaults {
struct Stats {
  size_t live = 0;
  size_t peak = 0;
  unsigned largeChunks = 0;
  unsigned smallChunks = 0;
  unsigned windowRejects = 0;
  unsigned windowAttempts = 0;
  unsigned canaryFailures = 0;
};
// Budget covers only actual ZIP/miniz mallocs, not the host's renderer/parser.
void reset(size_t budget = static_cast<size_t>(-1), bool rejectWindows = false, bool rejectFirstRead = false,
           void (*onWindowReject)(unsigned) = nullptr);
const Stats& stats();
void disable();
}  // namespace ZipInflateFaults
