#pragma once
#include "pocket_daily/home/HomeRenderer.h"

class UITheme {
 public:
  static UITheme& getInstance() {
    static UITheme theme;
    return theme;
  }
  static PocketDaily::Home::Metrics getMetrics() { return {}; }
};
