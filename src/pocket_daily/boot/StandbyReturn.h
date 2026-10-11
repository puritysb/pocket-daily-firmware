#pragma once

#include <cstdint>

namespace PocketDaily::Boot {
enum class StandbyReturn : uint32_t { None, Reader, SameWifi };
struct StandbyTicket {
  uint32_t magic = 0;
  StandbyReturn target = StandbyReturn::None;
  uint32_t check = 0;
};
inline void armStandbyReturn(StandbyTicket& ticket, StandbyReturn target) {
  ticket = {0x57414b31, target, 0x57414b31 ^ static_cast<uint32_t>(target)};
}
// Only an intentional software restart can consume this one-shot RTC route.
// Brownout, cold boot, watchdog and uninitialized RTC data never raise Wi-Fi.
inline StandbyReturn consumeStandbyReturn(StandbyTicket& ticket, bool softwareRestart) {
  const bool valid = softwareRestart && ticket.magic == 0x57414b31 &&
                     ticket.check == (ticket.magic ^ static_cast<uint32_t>(ticket.target)) &&
                     (ticket.target == StandbyReturn::Reader || ticket.target == StandbyReturn::SameWifi);
  const auto target = valid ? ticket.target : StandbyReturn::None;
  ticket = {};
  return target;
}
}  // namespace PocketDaily::Boot
