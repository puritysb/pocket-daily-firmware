#pragma once
#include <string_view>

namespace FsHelpers {
inline bool hasEpubExtension(std::string_view name) {
  return name.size() > 5 && name.substr(name.size() - 5) == ".epub";
}
}  // namespace FsHelpers
