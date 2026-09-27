#include <HalStorage.h>

#include <cstddef>
#include <cstring>

#include "pocket_daily/learning_pack.h"

// Header checks and single-record access, split from learning_pack.cpp so they
// build without mbedtls (host tests). Whole-pack SHA validation, backup
// recovery and installation stay in learning_pack.cpp.
namespace PocketDaily {
namespace LearningPack {
namespace {

constexpr char kMagic[] = {'P', 'D', 'L', 'P'};

uint32_t fnv32(const uint8_t* bytes, size_t length) {
  uint32_t hash = 2166136261U;
  for (size_t i = 0; i < length; ++i) {
    hash ^= bytes[i];
    hash *= 16777619U;
  }
  return hash;
}

template <size_t N>
bool terminated(const char (&value)[N]) {
  return memchr(value, '\0', N) != nullptr;
}

template <size_t N>
void terminate(char (&value)[N]) {
  value[N - 1] = '\0';
}

// The record at `index` of an open pack whose header already passed
// headerValid. The returned record is defensively terminated.
bool readRecordAt(HalFile& file, const Header& header, uint32_t index, Record& record) {
  if (index >= header.recordCount) return false;
  const uint32_t offset = sizeof(Header) + index * sizeof(Record);
  if (!file.seekSet(offset) || file.read(&record, sizeof(record)) != static_cast<int>(sizeof(record))) return false;
  terminate(record.glyph);
  terminate(record.onReading);
  terminate(record.kunReading);
  terminate(record.meaningKo);
  terminate(record.meaningEn);
  terminate(record.primaryWord);
  terminate(record.wordReading);
  terminate(record.wordMeaningKo);
  terminate(record.example);
  terminate(record.exampleMeaningKo);
  return record.itemId != 0 && record.glyph[0] != '\0';
}

}  // namespace

bool headerValid(const Header& header, size_t fileSize) {
  if (memcmp(header.magic, kMagic, sizeof(kMagic)) != 0 || header.formatVersion != FORMAT_VERSION ||
      header.headerSize != sizeof(Header) || header.recordSize != sizeof(Record) || header.recordCount == 0 ||
      header.totalBytes != fileSize || header.totalBytes > MAX_PACK_BYTES) {
    return false;
  }
  const uint64_t expected = static_cast<uint64_t>(sizeof(Header)) +
                            static_cast<uint64_t>(header.recordCount) * static_cast<uint64_t>(sizeof(Record));
  if (expected != header.totalBytes) return false;
  if (!terminated(header.packageId) || !terminated(header.locale) || !terminated(header.title) ||
      !terminated(header.licenseSpdx) || !terminated(header.sourceRevision) || !terminated(header.attribution)) {
    return false;
  }
  // A pack without an explicit licence and attribution must never become
  // active, even if every byte is otherwise valid.
  const bool acceptedLicense = strcmp(header.licenseSpdx, "CC0-1.0") == 0 ||
                               strcmp(header.licenseSpdx, "CC-BY-4.0") == 0 ||
                               strcmp(header.licenseSpdx, "CC-BY-SA-4.0") == 0;
  if (strcmp(header.packageId, PACKAGE_ID) != 0 || strcmp(header.locale, "ko-KR") != 0 || !header.title[0] ||
      !header.sourceRevision[0] || !acceptedLicense || !header.attribution[0]) {
    return false;
  }
  return header.headerFnv32 == fnv32(reinterpret_cast<const uint8_t*>(&header), offsetof(Header, headerFnv32));
}

bool readRecord(uint32_t index, Record& record) {
  memset(&record, 0, sizeof(record));
  HalFile file = Storage.open(PACK_PATH, O_RDONLY);
  if (!file) return false;
  Header header{};
  if (file.read(&header, sizeof(header)) != static_cast<int>(sizeof(header)) || !headerValid(header, file.size())) {
    return false;
  }
  return readRecordAt(file, header, index, record);
}

bool readDayRecord(uint32_t ordinal, Header& header, Record& record) {
  memset(&header, 0, sizeof(header));
  memset(&record, 0, sizeof(record));
  HalFile file = Storage.open(PACK_PATH, O_RDONLY);
  if (!file) return false;
  if (file.read(&header, sizeof(header)) != static_cast<int>(sizeof(header)) || !headerValid(header, file.size())) {
    return false;
  }
  return readRecordAt(file, header, ordinal % header.recordCount, record);
}

}  // namespace LearningPack
}  // namespace PocketDaily
