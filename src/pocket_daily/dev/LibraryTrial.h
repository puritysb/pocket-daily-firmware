#pragma once
#ifdef ENABLE_DEV_REMOTE_FLASH
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace PocketDaily::DevCapture {
inline constexpr uint32_t LIBRARY_WALK_ACTION = 6;
enum class LibraryPhase : uint32_t {
  Absent,
  Admitted,
  Original,
  Owned,
  Opening,
  Reading,
  Completed,
  Failed,
  Cleaning,
  Cleaned
};
enum class LibraryError : uint32_t {
  None,
  Index,
  OriginalMissing,
  OwnedMissing,
  Frame,
  Open,
  Interrupted,
  Budget,
  Persistence,
  Preservation,
  Cleanup
};
inline constexpr uint32_t ORIGINAL_ROW = 1, ORIGINAL_FRAME = 2, OWNED_ROW = 4, OWNED_FRAME = 8;
inline constexpr uint32_t LIBRARY_FRAMES = ORIGINAL_ROW | ORIGINAL_FRAME | OWNED_ROW | OWNED_FRAME;
// One developer-only, run-bound record. No per-row allocations or second frame.
struct LibraryEvidence {
  uint32_t magic = 0x4c425231, version = 1, run = 0;
  LibraryPhase phase = LibraryPhase::Absent;
  LibraryError error = LibraryError::None;
  uint32_t bytes = 0, pages = 0, flags = 0, opened = 0;
  uint32_t originalRow = UINT32_MAX, ownedRow = UINT32_MAX, elapsed = 0;
  char reference[128]{};
  uint8_t sourceSha[32]{}, titleSha[32]{}, backupSha[32]{}, preservedSha[32]{};
  uint32_t check = 0;
};
static_assert(sizeof(LibraryEvidence) <= 320);
inline uint32_t libraryChecksum(const LibraryEvidence& value) {
  uint32_t hash = 2166136261u;
  const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
  for (size_t i = 0; i < offsetof(LibraryEvidence, check); ++i) hash = (hash ^ bytes[i]) * 16777619u;
  return hash;
}
inline bool validLibraryReference(std::string_view path) {
  if (path.size() < 7 || path.size() >= sizeof(LibraryEvidence::reference) || path.front() != '/' ||
      !path.ends_with(".epub") || path.find("..") != std::string_view::npos || path.starts_with("/Pocket visual "))
    return false;
  for (size_t i = 1; i < path.size(); ++i) {
    const char c = path[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
          c == '.'))
      return false;
  }
  return true;
}
inline bool decodeLibraryDigest(std::string_view text, uint8_t* out) {
  if (text.size() != 64) return false;
  for (size_t i = 0; i < 32; ++i) {
    unsigned value = 0;
    for (size_t j = 0; j < 2; ++j) {
      const char c = text[i * 2 + j];
      if (c < '0' || (c > '9' && c < 'a') || c > 'f') return false;
      value = value * 16 + (c <= '9' ? c - '0' : c - 'a' + 10);
    }
    out[i] = static_cast<uint8_t>(value);
  }
  return true;
}
inline bool validLibraryEvidence(const LibraryEvidence& value, uint32_t run) {
  if (((value.flags & ORIGINAL_FRAME) && !(value.flags & ORIGINAL_ROW)) ||
      ((value.flags & OWNED_ROW) && !(value.flags & ORIGINAL_FRAME)) ||
      ((value.flags & OWNED_FRAME) && !(value.flags & OWNED_ROW)) || (value.opened && value.flags != LIBRARY_FRAMES) ||
      ((value.phase == LibraryPhase::Completed || value.phase == LibraryPhase::Cleaning ||
        value.phase == LibraryPhase::Cleaned) &&
       (value.opened != 1 || value.flags != LIBRARY_FRAMES)))
    return false;
  return run && value.magic == 0x4c425231 && value.version == 1 && value.run == run &&
         value.phase >= LibraryPhase::Admitted && value.phase <= LibraryPhase::Cleaned &&
         value.error <= LibraryError::Cleanup && value.bytes > 0 && value.bytes <= 4 * 1024 * 1024 && value.pages > 0 &&
         value.pages <= 128 && (value.flags & ~LIBRARY_FRAMES) == 0 && value.opened <= 1 &&
         memchr(value.reference, '\0', sizeof(value.reference)) && validLibraryReference(value.reference) &&
         value.check == libraryChecksum(value);
}
inline bool libraryRowFullyDrawn(int selected, int windowStart, int drawnRows, int itemOffset, int itemValue) {
  return selected >= windowStart && windowStart >= 0 && drawnRows > 0 && itemOffset >= 0 && itemOffset < drawnRows &&
         selected - windowStart == itemOffset && selected == itemValue;
}
inline bool libraryCanOpen(const LibraryEvidence& value, bool exactOwnedPath) {
  return exactOwnedPath && value.phase == LibraryPhase::Owned && value.error == LibraryError::None &&
         value.flags == LIBRARY_FRAMES && value.opened == 0;
}
inline bool libraryRequestMatches(bool present, uint32_t checkedRun, uint32_t run) {
  return run && (!present || checkedRun == run);
}
inline bool libraryTerminal(LibraryPhase phase) {
  return phase == LibraryPhase::Completed || phase == LibraryPhase::Failed || phase == LibraryPhase::Cleaning ||
         phase == LibraryPhase::Cleaned;
}
}  // namespace PocketDaily::DevCapture
#endif
