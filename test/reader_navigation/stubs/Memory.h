#pragma once
#include <memory>
#include <utility>
template <typename T, typename... Args>
std::unique_ptr<T> makeUniqueNoThrow(Args&&... args) {
  return std::make_unique<T>(std::forward<Args>(args)...);
}
