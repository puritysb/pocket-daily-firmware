#pragma once
#include <string>

namespace ReadingSyncTestHost {
inline std::string lastBreadcrumb;
}  // namespace ReadingSyncTestHost

struct HalSystem {
  static void feedWatchdogIfRegistered() {}
  static void setCrashBreadcrumb(const char* text) { ReadingSyncTestHost::lastBreadcrumb = text ? text : ""; }
};
