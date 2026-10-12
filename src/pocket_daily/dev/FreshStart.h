#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace PocketDaily::FreshStart {
// Explicit developer-only reset. Keep books/fonts and preserve old SD data;
// fixed destinations reject a second reset instead of overwriting the backup.
inline constexpr const char* SOURCES[] = {"/.crosspoint", "/.pocket", "/pocket-daily"};
inline constexpr const char* BACKUPS[] = {"/.first-install-crosspoint", "/.first-install-pocket",
                                          "/.first-install-content"};
enum class Result { Completed, BackupExists, ArchiveFailed, BondResetFailed, RollbackFailed };

template <typename Operations>
Result run(Operations& ops, uint32_t archiveRun = 0) {
  // Bounded local paths, no heap. Explicit run IDs preserve earlier rehearsals.
  char backups[3][48];
  for (size_t i = 0; i < 3; ++i) {
    if (archiveRun)
      snprintf(backups[i], sizeof(backups[i]), "%s-%lu", BACKUPS[i], static_cast<unsigned long>(archiveRun));
    else
      snprintf(backups[i], sizeof(backups[i]), "%s", BACKUPS[i]);
  }
  for (const auto& backup : backups) {
    if (ops.exists(backup)) return Result::BackupExists;
  }
  bool moved[3] = {};
  Result result = Result::Completed;
  for (size_t i = 0; i < 3; ++i) {
    if (!ops.exists(SOURCES[i])) continue;
    if (!ops.rename(SOURCES[i], backups[i])) {
      result = Result::ArchiveFailed;
      break;
    }
    moved[i] = true;
  }
  if (result == Result::Completed && !ops.clearBonds()) result = Result::BondResetFailed;
  if (result == Result::Completed) return result;
  // Roll back all possible moves even if an earlier rollback fails.
  for (size_t i = 3; i > 0; --i) {
    if (moved[i - 1] && !ops.rename(backups[i - 1], SOURCES[i - 1])) result = Result::RollbackFailed;
  }
  return result;
}
}  // namespace PocketDaily::FreshStart
