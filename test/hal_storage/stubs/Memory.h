#pragma once
#include <memory>
#include <new>
#include <utility>
namespace FakeMemory {
inline bool fail = false;
}
template <typename T, typename... Args>
std::unique_ptr<T> makeUniqueNoThrow(Args&&... args) {
  if (FakeMemory::fail) return {};
  return std::unique_ptr<T>(new (std::nothrow) T(std::forward<Args>(args)...));
}
