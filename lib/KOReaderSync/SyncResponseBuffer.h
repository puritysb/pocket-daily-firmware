#pragma once

#include <Memory.h>

#include <cstring>

// Allocate once before the handshake: callbacks only copy, never grow the heap.
// A 4 KiB cap covers the small KOSync JSON contract. A stack buffer would exceed
// the embedded local-buffer budget; a permanent buffer would tax normal reading.
class SyncResponseBuffer {
 public:
  static constexpr size_t CAPACITY = 4096;
  bool ready() const { return bytes != nullptr; }
  bool failed() const { return rejected; }
  const char* data() const { return bytes.get(); }
  bool append(const void* source, size_t count) {
    if (rejected || !bytes || (!source && count) || count > CAPACITY - used) {
      rejected = true;
      return false;
    }
    if (count) std::memcpy(bytes.get() + used, source, count);
    used += count;
    bytes[used] = '\0';
    return true;
  }

 private:
  std::unique_ptr<char[]> bytes = makeUniqueNoThrow<char[]>(CAPACITY + 1);
  size_t used = 0;
  bool rejected = false;
};
