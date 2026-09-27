#pragma once

// Keep the production allocator/cleanup definitions, instrumenting only the
// entry point to inject deterministic snapshot OOM in this test executable.
#define makeUniqueNoThrow realMakeUniqueNoThrow
#include "../../../lib/Memory/Memory.h"
#undef makeUniqueNoThrow

namespace FakeMemory {
inline size_t watchedSize = 0;
inline size_t attempts = 0;
inline bool refuse = false;
}  // namespace FakeMemory

template <typename T, typename... Args>
  requires(!std::is_array_v<T>)
std::unique_ptr<T> makeUniqueNoThrow(Args&&... args) {
  if (sizeof(T) == FakeMemory::watchedSize) {
    ++FakeMemory::attempts;
    if (FakeMemory::refuse) return nullptr;
  }
  return realMakeUniqueNoThrow<T>(std::forward<Args>(args)...);
}
