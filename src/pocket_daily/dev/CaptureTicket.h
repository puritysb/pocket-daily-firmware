#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace PocketDaily::DevCapture {
// On-SD admission bound; the EPUB parser streams bytes, never allocates this size.
inline constexpr bool validBookSize(size_t size) { return size > 0 && size <= 4 * 1024 * 1024; }
enum class ReaderAction : uint8_t { Capture, Seed, Accept, Advance };
enum class ReaderPhase : uint8_t { Reading, Accepted, Turned, Closing };
inline constexpr bool readyToCapture(ReaderAction action, ReaderPhase phase) {
  switch (action) {
    case ReaderAction::Capture:
    case ReaderAction::Seed:
      return phase == ReaderPhase::Reading;
    case ReaderAction::Accept:
      return phase == ReaderPhase::Accepted;
    case ReaderAction::Advance:
      return phase == ReaderPhase::Turned;
  }
  return false;
}
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
