#include "ZipInflateFaults.h"

#include <Arduino.h>
#include <BuildScratch.h>
#include <HalMemory.h>
#include <HalStorage.h>
#include <InflateStream.h>
#include <Logging.h>
#include <ZipFile.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdlib>
#include <cstring>

#include "MinizConfig.h"

namespace ZipInflateFaults {
namespace {
struct Allocation {
  void* ptr = nullptr;
  size_t size = 0;
};
std::array<Allocation, 16> allocations{};
Stats counters;
size_t limit;
bool enabled = false;
bool rejectWindow = false;
bool rejectRead = false;
void (*windowRejected)(unsigned) = nullptr;
}  // namespace

void reset(size_t budget, bool rejectWindows, bool rejectFirstRead, void (*onWindowReject)(unsigned)) {
  assert(counters.live == 0);
  counters = {};
  limit = budget;
  rejectWindow = rejectWindows;
  rejectRead = rejectFirstRead;
  windowRejected = onWindowReject;
  enabled = true;
}
const Stats& stats() { return counters; }
void disable() { enabled = false; }

void* allocate(size_t size, const char* function) {
  if (!enabled) return std::malloc(size);
  if (size == 32768) ++counters.windowAttempts;
  const bool streamBuffer = std::strcmp(function, "readFileToStream") == 0;
  if (streamBuffer && size == 8192) ++counters.largeChunks;
  if (streamBuffer && size == 1024) ++counters.smallChunks;
  if (streamBuffer && size == 8192 && rejectRead) {
    rejectRead = false;
    return nullptr;
  }
  if (size > limit - counters.live || (size == 32768 && rejectWindow)) {
    if (size == 32768) {
      ++counters.windowRejects;
      if (windowRejected) windowRejected(counters.windowRejects);
    }
    return nullptr;
  }
  auto* backing = static_cast<uint8_t*>(std::malloc(size + 16));
  if (!backing) return nullptr;
  std::memset(backing, 0xA5, 8);
  std::memset(backing + 8 + size, 0x5A, 8);
  void* result = backing + 8;
  for (auto& entry : allocations) {
    if (!entry.ptr) {
      entry = {result, size};
      counters.live += size;
      counters.peak = std::max(counters.peak, counters.live);
      return result;
    }
  }
  std::abort();  // The real stream uses only four simultaneous allocations.
}
void release(void* ptr) {
  if (!ptr) return;
  for (auto& entry : allocations) {
    if (entry.ptr == ptr) {
      auto* backing = static_cast<uint8_t*>(ptr) - 8;
      for (size_t i = 0; i < 8; ++i) {
        if (backing[i] != 0xA5 || backing[8 + entry.size + i] != 0x5A) ++counters.canaryFailures;
      }
      counters.live -= entry.size;
      entry = {};
      std::free(backing);
      return;
    }
  }
  std::free(ptr);
}
}  // namespace ZipInflateFaults

// Same implementations as firmware, with faults limited to their malloc/free calls.
#define malloc(size) ZipInflateFaults::allocate(size, __func__)
#define free ZipInflateFaults::release
#include "../../lib/ZipFile/ZipFile.cpp"
#include "../../lib/miniz/src/InflateStream.cpp"
#undef free
#undef malloc
