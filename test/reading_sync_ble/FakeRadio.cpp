#include "FakeRadio.h"

#include <cstdio>
#include <cstring>

#include "pocket_daily/nearby_sync/NearbySyncService.h"

namespace Pocket::NearbySync {

bool Service::begin(const char*, const char*, const Mode requestedMode, uint32_t, bool) {
  mode_ = requestedMode;
  snprintf(deviceId_, sizeof(deviceId_), "%s", "89ABCDEF");
  snprintf(advertisedName_, sizeof(advertisedName_), "Pocket-%.4s", deviceId_ + 4);
  connections_.store(0, std::memory_order_release);
  queue_.clear();
  running_ = true;
  return true;
}

void Service::end() {
  running_ = false;
  connected_.store(false);
  authenticated_.store(false);
  queue_.clear();
}

bool Service::notifyRecord(const char* record) {
  if (!record || !isConnected() || !isAuthenticated()) return false;
  if (!FakeRadio::acceptNotifications) {
    ++FakeRadio::rejectedNotifications;
    return false;
  }
  FakeRadio::sent.emplace_back(record);
  return true;
}

void Service::disconnect() { ++FakeRadio::disconnects; }

}  // namespace Pocket::NearbySync
