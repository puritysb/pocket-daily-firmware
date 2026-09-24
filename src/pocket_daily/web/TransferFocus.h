#pragma once

#include <cstdint>

namespace PocketDaily::Web {

// Keep optional services out of the upload -> verification -> next-file gap.
// Allocation-free and wrap-safe; ticked even while no WS listener exists.
class TransferFocus {
 public:
  static constexpr uint32_t QUIET_MS = 5000;
  void begin() { active_ = true; }
  void end(uint32_t now) {
    active_ = false;
    cooling_ = true;
    endedAt_ = now;
  }
  bool allowsListener(uint32_t now) const {
    return !active_ && (!cooling_ || static_cast<uint32_t>(now - endedAt_) >= QUIET_MS);
  }

 private:
  bool active_ = false;
  bool cooling_ = false;
  uint32_t endedAt_ = 0;
};

}  // namespace PocketDaily::Web
