#pragma once
#include <cstdint>

namespace Pocket::KeyboardRemote {
inline constexpr uint32_t MIN_FREE = 50U * 1024U;
inline constexpr uint32_t MIN_BLOCK = 16U * 1024U;
inline constexpr uint32_t START_FREE = MIN_FREE + 80U * 1024U;
inline constexpr uint32_t IDLE_MS = 5U * 60U * 1000U;
inline constexpr uint32_t SESSION_MS = 2U * 60U * 60U * 1000U;
inline bool memoryAllowed(uint32_t free, uint32_t block, bool starting) {
  return free >= (starting ? START_FREE : MIN_FREE) && block >= (starting ? 40U * 1024U : MIN_BLOCK);
}
inline bool expired(uint32_t now, uint32_t lastInput, uint32_t began) {
  return now - lastInput >= IDLE_MS || now - began >= SESSION_MS;
}
inline bool readyAllowed(uint32_t readyId, uint32_t highest, bool pending) {
  return (readyId == 0 && highest == 0) || (readyId != 0 && readyId == highest && pending);
}
enum class Admission : uint8_t { Accept, Busy, Replay, Stale };
// Monotonic IDs bind keypresses to one connection. No old turn can run twice,
// even after it falls out of an arbitrary replay-cache window.
inline Admission admit(uint32_t sequence, uint32_t highest, bool pending, bool pageReady, bool hasReply) {
  if (sequence <= highest) return sequence == highest && !pending && hasReply ? Admission::Replay : Admission::Stale;
  if (pending || !pageReady) return Admission::Busy;
  return Admission::Accept;
}
}  // namespace Pocket::KeyboardRemote
