#pragma once
#include <memory>
#include <new>
#include <type_traits>
namespace FakeFontMemory {
inline bool fail = false;
inline size_t maxRequest = 0;
}  // namespace FakeFontMemory
template <typename T>
  requires std::is_unbounded_array_v<T>
std::unique_ptr<T> makeUniqueNoThrow(size_t count) {
  using Element = std::remove_extent_t<T>;
  if (count * sizeof(Element) > FakeFontMemory::maxRequest) FakeFontMemory::maxRequest = count * sizeof(Element);
  if (FakeFontMemory::fail) return {};
  return std::unique_ptr<T>(new (std::nothrow) Element[count]());
}
