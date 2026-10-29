#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace PocketDaily::DevCapture {
// On-SD admission bound; the EPUB parser streams bytes, never allocates this size.
inline constexpr bool validBookSize(size_t size) { return size > 0 && size <= 4 * 1024 * 1024; }
enum class ReaderAction : uint8_t { Capture, Seed, Accept, Advance, Walk };
// Persisted action5 requires paint evidence even if both paint files are missing.
// Public action=walk and the in-memory ReaderAction remain unchanged.
inline constexpr uint32_t PAINT_WALK_ACTION = 5;
inline constexpr uint32_t LIBRARY_ACTION_TICKET = 6;
inline constexpr bool isWalkActionTicket(uint32_t action) {
  return action == static_cast<uint32_t>(ReaderAction::Walk) || action == PAINT_WALK_ACTION ||
         action == LIBRARY_ACTION_TICKET;
}
inline constexpr bool walkRecoveryMatches(uint32_t action, uint32_t request, bool valid, uint32_t storedRun) {
  return !isWalkActionTicket(action) || (request && valid && storedRun == request);
}
inline constexpr bool paintEvidenceRequired(uint32_t action, bool markerExists, bool recordExists) {
  return action == PAINT_WALK_ACTION || action == LIBRARY_ACTION_TICKET || markerExists || recordExists;
}
inline constexpr bool paintAllowsCleanup(uint32_t action, bool markerExists, bool recordExists, bool validComplete) {
  return isWalkActionTicket(action) && (!paintEvidenceRequired(action, markerExists, recordExists) || validComplete);
}
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
    case ReaderAction::Walk:
      return false;  // Walk completion uses actual rendered-page evidence.
  }
  return false;
}
struct Ticket {
  uint32_t run, check;
};
inline constexpr Ticket ticketFor(uint32_t run) { return {run, run ^ 0x56495331u}; }
// First-install resets archive /.pocket. Reader trials may precede the first
// framebuffer capture, so ticket creation must not depend on that capture.
template <typename StorageType, typename FileType>
bool writeTicketTo(StorageType& storage, const char* path, FileType& file, uint32_t run) {
  if (!storage.exists("/.pocket") && !storage.mkdir("/.pocket")) return false;
  const Ticket ticket = ticketFor(run);
  return storage.openFileForWrite("VISUAL", path, file) &&
         file.write(reinterpret_cast<const uint8_t*>(&ticket), sizeof(ticket)) == sizeof(ticket);
}
// SD bytes can be unaligned; reject partial/old/corrupt records before use.
inline uint32_t decodeTicket(const void* data, size_t size) {
  if (!data || size != sizeof(Ticket)) return 0;
  Ticket ticket{};
  memcpy(&ticket, data, sizeof(ticket));
  return ticket.check == (ticket.run ^ 0x56495331u) ? ticket.run : 0;
}
}  // namespace PocketDaily::DevCapture
