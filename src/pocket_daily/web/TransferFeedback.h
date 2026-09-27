#pragma once

#include <cstdint>
#include <string_view>

namespace PocketDaily::Web {
// A snapshot for the existing Sync screen. No file names, dynamic allocation,
// extra framebuffer or radio service; the activity copies it under RenderLock.
enum class TransferPhase : uint8_t { Idle, Ready, Receiving, Verifying, Saved, Paused, Removed, Failed };
enum class TransferKind : uint8_t { Content, Firmware };
struct TransferFeedback {
  TransferPhase phase = TransferPhase::Idle;
  TransferKind kind = TransferKind::Content;
  uint32_t received = 0;
  uint32_t total = 0;
  unsigned percent() const {
    if (total == 0) return 0;
    const auto value = static_cast<uint64_t>(received) * 100 / total;
    return value > 100 ? 100 : static_cast<unsigned>(value);
  }
};

// Cleanup accepts exactly a generated UUID staging basename, never update.bin,
// a book, a backup or an arbitrary hidden file. Parent policy is checked by the route.
inline bool isTransferStagingName(std::string_view name) {
  if (name.size() != 49 || name.substr(0, 8) != ".pocket-" || name.substr(44) != ".part") return false;
  for (size_t i = 0; i < 36; ++i) {
    const char c = name[8 + i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') return false;
    } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}
}  // namespace PocketDaily::Web
