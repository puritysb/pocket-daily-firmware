#include "ReadingProgressStore.h"

#include <HalStorage.h>

#include <cstdio>
#include <cstring>

namespace PocketDaily::ReadingProgress {
namespace {
// Cache paths are "/.crosspoint/epub_<hash>" (~40 bytes); the longest name adds 26.
constexpr size_t PATH_BYTES = 96;

bool pathFor(const char* cachePath, const char* file, const char* suffix, char (&out)[PATH_BYTES]) {
  const int n = snprintf(out, sizeof(out), "%s%s%s", cachePath ? cachePath : "", file, suffix);
  return cachePath && cachePath[0] == '/' && n > 0 && static_cast<size_t>(n) < sizeof(out);
}

size_t readFile(const char* path, Scratch& scratch) {
  HalFile file = Storage.open(path, O_RDONLY);
  if (!file) return 0;
  const size_t size = file.size();
  if (size == 0 || size > sizeof(scratch.bytes)) return 0;
  return file.read(scratch.bytes, size) == static_cast<int>(size) ? size : 0;
}

bool writeAtomic(const char* cachePath, const char* file, const Scratch& scratch, const size_t size) {
  char finalPath[PATH_BYTES];
  char tempPath[PATH_BYTES];
  if (size == 0 || !pathFor(cachePath, file, "", finalPath) || !pathFor(cachePath, file, ".tmp", tempPath)) {
    return false;
  }
  {
    HalFile out = Storage.open(tempPath, O_WRITE | O_CREAT | O_TRUNC);
    if (!out || out.write(scratch.bytes, size) != size) {
      if (out) out.close();
      Storage.remove(tempPath);
      return false;
    }
    out.close();  // SdFat must not rename a path that is still open.
  }
  Storage.remove(finalPath);
  return Storage.rename(tempPath, finalPath);
}
}  // namespace

bool loadRecord(const char* cachePath, Record& record, Scratch& scratch) {
  char path[PATH_BYTES];
  if (!pathFor(cachePath, RECORD_FILE, "", path)) return false;
  const size_t size = readFile(path, scratch);
  return size && decodeRecord(scratch.bytes, size, record);
}

bool saveRecord(const char* cachePath, const Record& record, Scratch& scratch) {
  uint8_t* bytes = scratch.bytes;  // encoded in place, then written from the same buffer
  const size_t size = encodeRecord(record, bytes, sizeof(scratch.bytes));
  return writeAtomic(cachePath, RECORD_FILE, scratch, size);
}

bool loadOffer(const char* cachePath, Offer& offer, Scratch& scratch) {
  char path[PATH_BYTES];
  if (!pathFor(cachePath, OFFER_FILE, "", path)) return false;
  const size_t size = readFile(path, scratch);
  return size && decodeOffer(scratch.bytes, size, offer);
}

bool saveOffer(const char* cachePath, const Offer& offer, Scratch& scratch) {
  uint8_t* bytes = scratch.bytes;  // encoded in place, then written from the same buffer
  const size_t size = encodeOffer(offer, bytes, sizeof(scratch.bytes));
  return writeAtomic(cachePath, OFFER_FILE, scratch, size);
}

bool hasOffer(const char* cachePath) {
  char path[PATH_BYTES];
  return pathFor(cachePath, OFFER_FILE, "", path) && Storage.exists(path);
}

void removeOffer(const char* cachePath) {
  char path[PATH_BYTES];
  if (pathFor(cachePath, OFFER_FILE, "", path)) Storage.remove(path);
}

uint32_t nextSequence() {
  uint8_t bytes[8] = {};
  uint32_t value = 0;
  {
    HalFile in = Storage.open(SEQUENCE_PATH, O_RDONLY);
    if (in && in.size() == sizeof(bytes) && in.read(bytes, sizeof(bytes)) == static_cast<int>(sizeof(bytes))) {
      uint32_t stored = 0;
      uint32_t check = 0;
      for (int i = 3; i >= 0; --i) {
        stored = (stored << 8) | bytes[i];
        check = (check << 8) | bytes[4 + i];
      }
      if (check == ~stored) value = stored;
    }
  }
  value = value == UINT32_MAX ? 1 : value + 1;
  const uint32_t check = ~value;
  for (int i = 0; i < 4; ++i) {
    bytes[i] = static_cast<uint8_t>(value >> (8 * i));
    bytes[4 + i] = static_cast<uint8_t>(check >> (8 * i));
  }
  HalFile out = Storage.open(SEQUENCE_PATH, O_WRITE | O_CREAT | O_TRUNC);
  if (out) out.write(bytes, sizeof(bytes));
  return value;
}
}  // namespace PocketDaily::ReadingProgress
