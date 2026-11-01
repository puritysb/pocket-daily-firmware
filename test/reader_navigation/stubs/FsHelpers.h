#pragma once
#include <string>
namespace FsHelpers {
inline bool hasBmpExtension(const std::string& p) { return p.ends_with(".bmp"); }
inline bool hasPngExtension(const std::string& p) { return p.ends_with(".png"); }
}  // namespace FsHelpers
