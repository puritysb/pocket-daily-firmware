#pragma once
// Test double for the NimBLE half of Pocket::NearbySync::Service
// (NearbySyncService.cpp). The record half (NearbySyncRecords.cpp) is real.
#include <string>
#include <vector>

namespace FakeRadio {
inline std::vector<std::string> sent;  // notifications the stack accepted
inline bool acceptNotifications = true;
inline int rejectedNotifications = 0;
inline int disconnects = 0;
inline void reset() {
  sent.clear();
  acceptNotifications = true;
  rejectedNotifications = 0;
  disconnects = 0;
}
}  // namespace FakeRadio
