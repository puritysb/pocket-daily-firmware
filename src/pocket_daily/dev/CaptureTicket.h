#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace PocketDaily::DevCapture {
struct Ticket {
  uint32_t run, check;
};
inline constexpr Ticket ticketFor(uint32_t run) { return {run, run ^ 0x56495331u}; }
// SD bytes can be unaligned; reject partial/old/corrupt records before use.
inline uint32_t decodeTicket(const void* data, size_t size) {
  if (!data || size != sizeof(Ticket)) return 0;
  Ticket ticket{};
  memcpy(&ticket, data, sizeof(ticket));
  return ticket.check == (ticket.run ^ 0x56495331u) ? ticket.run : 0;
}
}  // namespace PocketDaily::DevCapture
