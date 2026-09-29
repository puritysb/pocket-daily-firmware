#pragma once
#include <string>
class GfxRenderer {
 public:
  void clearFallbackCache() const {}
  void appendFallbackCodepoints(int, const char*, std::string&) const {}
  void prewarmFallbackFont(const char*) const {}
};
