#pragma once
#include <cstddef>
#include <functional>
namespace freeink {
enum class MemPool : unsigned char { Internal, Psram, Default };
struct CacheSink {
  const char* name;
  unsigned char priority;
  std::function<size_t(size_t)> evict;
};
// Host allocations do not model the device's contiguous heap or cache pressure.
class MemoryManager {
 public:
  static MemoryManager& instance() {
    static MemoryManager manager;
    return manager;
  }
  bool ensureFree(size_t, MemPool = MemPool::Default) { return true; }
  int registerSink(const CacheSink&) { return 0; }
  void unregisterSink(const char*) {}
  size_t freeBytes(MemPool = MemPool::Default) const { return 1024 * 1024; }
};
}  // namespace freeink
