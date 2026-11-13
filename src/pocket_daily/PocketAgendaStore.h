#pragma once

#include <cstdint>

#include "PocketAgenda.h"

// SD persistence for the app-provided glance (docs/pocket-agenda-v1.md). One
// fixed 1343-byte record; nothing is kept in RAM here. Pocket Daily loads it into
// its own (heap-allocated) activity when it is entered.
namespace PocketDaily::Agenda {
inline constexpr char AGENDA_PATH[] = "/.crosspoint/pocket-agenda.bin";
inline constexpr char AGENDA_TEMP_PATH[] = "/.crosspoint/pocket-agenda.tmp";
inline constexpr char AGENDA_BACKUP_PATH[] = "/.crosspoint/pocket-agenda.bak";

enum class SaveResult : uint8_t { Ok, Invalid, OutOfMemory, StorageError };

// Writes the temp file, reads it back and verifies it, then swaps it in; the
// previous record stays in place (or as the backup) until the new one is
// verified. Any failure leaves the previous glance loadable.
SaveResult save(const Snapshot& snapshot);

// Newest verified record: the active file, else the backup an interrupted
// save left behind. False (snapshot cleared) when neither is valid.
bool load(Snapshot& snapshot);
}  // namespace PocketDaily::Agenda
