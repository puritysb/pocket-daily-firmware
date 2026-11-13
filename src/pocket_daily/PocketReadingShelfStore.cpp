#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstring>

#include "PocketReadingShelf.h"

namespace PocketDaily::ReadingShelf {
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
  Selection checked;
};
}  // namespace

SaveResult save(const Selection& snapshot) {
  auto buffers = makeUniqueNoThrow<SaveBuffers>();
  if (!buffers) {
    LOG_ERR("PDSH", "Allocation failed: %u bytes", unsigned(sizeof(SaveBuffers)));
    return SaveResult::OutOfMemory;
  }
  if (!encode(snapshot, buffers->encoded.bytes)) return SaveResult::Invalid;
  const bool activeValid =
      readBytes(PATH, buffers->readBack) && decode(buffers->readBack.bytes, RECORD_BYTES, buffers->checked);
  if (activeValid && memcmp(buffers->readBack.bytes, buffers->encoded.bytes, RECORD_BYTES) == 0) return SaveResult::Ok;
  Storage.remove(TEMP_PATH);
  if (!writeBytes(TEMP_PATH, buffers->encoded) || !verify(TEMP_PATH, buffers->encoded, buffers->readBack)) {
    Storage.remove(TEMP_PATH);
    return SaveResult::StorageError;
  }
  // Keep the previous record as the backup until the new one is in place and
  // verified; load() falls back to it if power is lost in between.
  // A corrupt active file must never replace the only valid backup.
  if (activeValid && Storage.exists(BACKUP_PATH) && !Storage.remove(BACKUP_PATH)) return SaveResult::StorageError;
  const bool hadActive = activeValid;
  if (!activeValid && Storage.exists(PATH) && !Storage.remove(PATH)) return SaveResult::StorageError;
  if (hadActive && !Storage.rename(PATH, BACKUP_PATH)) {
    Storage.remove(TEMP_PATH);
    return SaveResult::StorageError;
  }
  if (!Storage.rename(TEMP_PATH, PATH) || !verify(PATH, buffers->encoded, buffers->readBack)) {
    Storage.remove(PATH);
    Storage.remove(TEMP_PATH);
    if (hadActive) Storage.rename(BACKUP_PATH, PATH);
    return SaveResult::StorageError;
  }
  Storage.remove(BACKUP_PATH);
  return SaveResult::Ok;
}

bool load(Selection& snapshot) {
  auto record = makeUniqueNoThrow<Record>();
  if (!record) {
    LOG_ERR("PDSH", "Allocation failed: %u bytes", unsigned(sizeof(Record)));
    snapshot = {};
    return false;
  }
  if ((readBytes(PATH, *record) && decode(record->bytes, RECORD_BYTES, snapshot)) ||
      (readBytes(BACKUP_PATH, *record) && decode(record->bytes, RECORD_BYTES, snapshot)))
    return true;
  snapshot = {};
  return false;
}
}  // namespace PocketDaily::ReadingShelf
