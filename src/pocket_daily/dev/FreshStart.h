#pragma once
#include <cstddef>

namespace PocketDaily::FreshStart {
// Explicit developer-only reset. Keep books/fonts and preserve old SD data;
// fixed destinations reject a second reset instead of overwriting the backup.
inline constexpr const char* SOURCES[] = {"/.crosspoint", "/.pocket", "/pocket-daily"};
inline constexpr const char* BACKUPS[] = {"/.first-install-crosspoint", "/.first-install-pocket",
                                          "/.first-install-content"};
enum class Result { Completed, BackupExists, ArchiveFailed, BondResetFailed, RollbackFailed };

template <typename Operations>
Result run(Operations& ops) {
  for (const auto* backup : BACKUPS) {
    if (ops.exists(backup)) return Result::BackupExists;
  }
  bool moved[3] = {};
  Result result = Result::Completed;
  for (size_t i = 0; i < 3; ++i) {
    if (!ops.exists(SOURCES[i])) continue;
    if (!ops.rename(SOURCES[i], BACKUPS[i])) {
      result = Result::ArchiveFailed;
      break;
    }
    moved[i] = true;
  }
  if (result == Result::Completed && !ops.clearBonds()) result = Result::BondResetFailed;
  if (result == Result::Completed) return result;
  // Roll back all possible moves even if an earlier rollback fails.
  for (size_t i = 3; i > 0; --i) {
    if (moved[i - 1] && !ops.rename(BACKUPS[i - 1], SOURCES[i - 1])) result = Result::RollbackFailed;
  }
  return result;
}
}  // namespace PocketDaily::FreshStart
