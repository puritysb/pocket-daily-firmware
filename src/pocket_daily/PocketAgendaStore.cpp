#include "PocketAgendaStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstring>

namespace PocketDaily::Agenda {
namespace {
// Entry/request-scoped record buffers live on the heap, never a task stack,
// and are released before the caller returns.
struct Record {
  uint8_t bytes[RECORD_BYTES];
};

bool readBytes(const char* path, Record& record) {
  HalFile file = Storage.open(path, O_RDONLY);
  return file && file.size() == RECORD_BYTES && file.read(record.bytes, RECORD_BYTES) == static_cast<int>(RECORD_BYTES);
}

bool writeBytes(const char* path, const Record& record) {
  HalFile file = Storage.open(path, O_WRITE | O_CREAT | O_TRUNC);
  return file && file.write(record.bytes, RECORD_BYTES) == RECORD_BYTES;
}

// The file must hold exactly the bytes that were encoded from a valid snapshot.
bool verify(const char* path, const Record& expected, Record& scratch) {
  return readBytes(path, scratch) && memcmp(scratch.bytes, expected.bytes, RECORD_BYTES) == 0;
}

struct SaveBuffers {
  Record encoded;
  Record readBack;
  Snapshot checked;
};
}  // namespace

SaveResult save(const Snapshot& snapshot) {
  auto buffers = makeUniqueNoThrow<SaveBuffers>();
  if (!buffers) {
    LOG_ERR("PDAG", "Allocation failed: %u bytes", unsigned(sizeof(SaveBuffers)));
    return SaveResult::OutOfMemory;
  }
  if (!encodeRecord(snapshot, buffers->encoded.bytes)) return SaveResult::Invalid;
  const bool activeValid = readBytes(AGENDA_PATH, buffers->readBack) &&
                           decodeRecord(buffers->readBack.bytes, RECORD_BYTES, buffers->checked);
  if (activeValid && memcmp(buffers->readBack.bytes, buffers->encoded.bytes, RECORD_BYTES) == 0) return SaveResult::Ok;
  Storage.remove(AGENDA_TEMP_PATH);
  if (!writeBytes(AGENDA_TEMP_PATH, buffers->encoded) ||
      !verify(AGENDA_TEMP_PATH, buffers->encoded, buffers->readBack)) {
    Storage.remove(AGENDA_TEMP_PATH);
    return SaveResult::StorageError;
  }
  // Keep the previous record as the backup until the new one is in place and
  // verified; load() falls back to it if power is lost in between.
  // Preserve a valid backup when an interrupted write left corrupt active bytes.
  if (activeValid && Storage.exists(AGENDA_BACKUP_PATH) && !Storage.remove(AGENDA_BACKUP_PATH))
    return SaveResult::StorageError;
  const bool hadActive = activeValid;
  if (!activeValid && Storage.exists(AGENDA_PATH) && !Storage.remove(AGENDA_PATH)) return SaveResult::StorageError;
  if (hadActive && !Storage.rename(AGENDA_PATH, AGENDA_BACKUP_PATH)) {
    Storage.remove(AGENDA_TEMP_PATH);
    return SaveResult::StorageError;
  }
  if (!Storage.rename(AGENDA_TEMP_PATH, AGENDA_PATH) || !verify(AGENDA_PATH, buffers->encoded, buffers->readBack)) {
    Storage.remove(AGENDA_PATH);
    Storage.remove(AGENDA_TEMP_PATH);
    if (hadActive) Storage.rename(AGENDA_BACKUP_PATH, AGENDA_PATH);
    return SaveResult::StorageError;
  }
  Storage.remove(AGENDA_BACKUP_PATH);
  return SaveResult::Ok;
}

bool load(Snapshot& snapshot) {
  auto record = makeUniqueNoThrow<Record>();
  if (!record) {
    LOG_ERR("PDAG", "Allocation failed: %u bytes", unsigned(sizeof(Record)));
    snapshot.clear();
    return false;
  }
  if ((readBytes(AGENDA_PATH, *record) && decodeRecord(record->bytes, RECORD_BYTES, snapshot)) ||
      (readBytes(AGENDA_BACKUP_PATH, *record) && decodeRecord(record->bytes, RECORD_BYTES, snapshot)))
    return true;
  snapshot.clear();
  return false;
}
}  // namespace PocketDaily::Agenda
