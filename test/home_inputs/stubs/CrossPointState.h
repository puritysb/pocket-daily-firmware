#pragma once
#include <string>

struct CrossPointState {
  std::string openEpubPath;
  static CrossPointState& getInstance() {
    static CrossPointState state;
    return state;
  }
};
#define APP_STATE CrossPointState::getInstance()
