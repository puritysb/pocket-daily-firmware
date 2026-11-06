#pragma once
#include <string>
namespace UrlUtils {
inline std::string buildUrl(const std::string& base, const std::string& ref) { return base + ref; }
}  // namespace UrlUtils
