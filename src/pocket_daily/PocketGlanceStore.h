#pragma once

#include <cstdint>

#include "PocketGlance.h"

// SD persistence for the app-provided glance (docs/pocket-glance-v1.md). One
// fixed 404-byte record; nothing is kept in RAM here. Pocket Daily loads it into
// its own (heap-allocated) activity when it is entered.
namespace PocketDaily::AppGlance {
inline constexpr char GLANCE_PATH[] = "/.crosspoint/pocket-glance.bin";
inline constexpr char GLANCE_TEMP_PATH[] = "/.crosspoint/pocket-glance.tmp";
inline constexpr char GLANCE_BACKUP_PATH[] = "/.crosspoint/pocket-glance.bak";

enum class SaveResult : uint8_t { Ok, Invalid, OutOfMemory, StorageError };

// Writes the temp file, reads it back and verifies it, then swaps it in; the
// previous record stays in place (or as the backup) until the new one is
// verified. Any failure leaves the previous glance loadable.
SaveResult save(const Snapshot& snapshot);

// Newest verified record: the active file, else the backup an interrupted
// save left behind. False (snapshot cleared) when neither is valid.
bool load(Snapshot& snapshot);
}  // namespace PocketDaily::AppGlance
