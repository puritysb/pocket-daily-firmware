#pragma once
#include <string>

#include "pocket_daily/home/HomeRenderer.h"

class UITheme {
 public:
  static UITheme& getInstance() {
    static UITheme theme;
    return theme;
  }
  struct Metrics : PocketDaily::Home::Metrics {
    int homeCoverHeight = 320;
  };
  static Metrics getMetrics() { return {}; }
  static std::string getCoverThumbPath(std::string path, int height) {
    const auto pos = path.find("[HEIGHT]");
    if (pos != std::string::npos) path.replace(pos, 8, std::to_string(height));
    return path;
  }
};
