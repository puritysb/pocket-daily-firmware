#include "PocketGlanceStore.h"

#include <HalStorage.h>
#include <Memory.h>

#include <cstring>

namespace PocketDaily::AppGlance {
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
};
}  // namespace

SaveResult save(const Snapshot& snapshot) {
  auto buffers = makeUniqueNoThrow<SaveBuffers>();
  if (!buffers) return SaveResult::OutOfMemory;
  if (!encodeRecord(snapshot, buffers->encoded.bytes)) return SaveResult::Invalid;
  Storage.remove(GLANCE_TEMP_PATH);
  if (!writeBytes(GLANCE_TEMP_PATH, buffers->encoded) ||
      !verify(GLANCE_TEMP_PATH, buffers->encoded, buffers->readBack)) {
    Storage.remove(GLANCE_TEMP_PATH);
    return SaveResult::StorageError;
  }
  // Keep the previous record as the backup until the new one is in place and
  // verified; load() falls back to it if power is lost in between.
  Storage.remove(GLANCE_BACKUP_PATH);
  const bool hadActive = Storage.exists(GLANCE_PATH);
  if (hadActive && !Storage.rename(GLANCE_PATH, GLANCE_BACKUP_PATH)) {
    Storage.remove(GLANCE_TEMP_PATH);
    return SaveResult::StorageError;
  }
  if (!Storage.rename(GLANCE_TEMP_PATH, GLANCE_PATH) || !verify(GLANCE_PATH, buffers->encoded, buffers->readBack)) {
    Storage.remove(GLANCE_PATH);
    Storage.remove(GLANCE_TEMP_PATH);
    if (hadActive) Storage.rename(GLANCE_BACKUP_PATH, GLANCE_PATH);
    return SaveResult::StorageError;
  }
  Storage.remove(GLANCE_BACKUP_PATH);
  return SaveResult::Ok;
}

bool load(Snapshot& snapshot) {
  auto record = makeUniqueNoThrow<Record>();
  if (record && ((readBytes(GLANCE_PATH, *record) && decodeRecord(record->bytes, RECORD_BYTES, snapshot)) ||
                 (readBytes(GLANCE_BACKUP_PATH, *record) && decodeRecord(record->bytes, RECORD_BYTES, snapshot))))
    return true;
  snapshot.clear();
  return false;
}
}  // namespace PocketDaily::AppGlance
