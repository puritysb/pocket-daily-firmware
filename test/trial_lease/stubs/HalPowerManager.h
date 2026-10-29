#pragma once
#include <cstdint>
namespace Fake {
inline bool sleepSupported = true;
inline uint32_t sleepSeconds = 0;
}  // namespace Fake
struct HalPowerManager {
  bool armRecoveryTimer(uint32_t seconds) {
    Fake::sleepSeconds = seconds;
    return Fake::sleepSupported;
  }
};
inline HalPowerManager powerManager;
