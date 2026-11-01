#pragma once
#include "activities/ActivityManager.h"
struct ReaderActivity : Activity {
  static inline ActivityManager* owner = nullptr;
  static inline ReaderNavigation observed{};
  static inline bool fail = false;
  void onEnter() override { observed = {owner->readerReturn, owner->shell}; }
  static std::unique_ptr<Activity> create(int&, int&, std::string, bool) {
    return fail ? nullptr : std::make_unique<ReaderActivity>();
  }
};
