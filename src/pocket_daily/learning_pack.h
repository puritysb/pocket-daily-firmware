#pragma once

#include <cstddef>
#include <cstdint>

namespace PocketDaily {
namespace LearningPack {

inline constexpr char PACKAGE_ID[] = "jp-n3-ko";
inline constexpr char PACK_DIR[] = "/pocket-daily/learning";
inline constexpr char PACK_PATH[] = "/pocket-daily/learning/jp-n3-ko.pdl";
inline constexpr char PACK_TEMP_PATH[] = "/pocket-daily/learning/jp-n3-ko.download";
inline constexpr char PACK_BACKUP_PATH[] = "/pocket-daily/learning/jp-n3-ko.bak";
inline constexpr uint16_t FORMAT_VERSION = 1;
inline constexpr uint32_t MAX_PACK_BYTES = 16U * 1024U * 1024U;

#pragma pack(push, 1)
// Fixed header shared with scripts/build-learning-pack.py. The payload is a
// direct-indexed array of fixed records, so the no-PSRAM C3 reads one lesson at
// a time instead of parsing or retaining a multi-megabyte JSON document.
struct Header {
  char magic[4];              // "PDLP"
  uint16_t formatVersion;     // FORMAT_VERSION
  uint16_t headerSize;        // sizeof(Header)
  uint16_t recordSize;        // sizeof(Record)
  uint16_t flags;
  uint32_t recordCount;
  uint32_t contentVersion;    // monotonically increasing pack revision
  uint32_t totalBytes;
  char packageId[32];
  char locale[16];
  char title[48];
  char licenseSpdx[32];
  char sourceRevision[40];
  char attribution[160];
  uint8_t payloadSha256[32];
  uint32_t headerFnv32;       // FNV-1a of all preceding header bytes
};

struct Record {
  uint32_t itemId;
  uint8_t level;  // 1=N5 foundation ... 3=N3-oriented
  uint8_t flags;
  uint16_t reserved;
  char glyph[8];
  char onReading[64];
  char kunReading[64];
  char meaningKo[96];
  char meaningEn[96];
  char primaryWord[48];
  char wordReading[64];
  char wordMeaningKo[96];
  char example[192];
  char exampleMeaningKo[192];
};
#pragma pack(pop)

static_assert(sizeof(Header) == 388, "learning pack header is a disk contract");
static_assert(sizeof(Record) == 928, "learning pack record is a disk contract");

struct Metadata {
  uint32_t contentVersion = 0;
  uint32_t recordCount = 0;
  uint32_t totalBytes = 0;
  char packageId[32] = {0};
  char locale[16] = {0};
  char title[48] = {0};
  char licenseSpdx[32] = {0};
  char attribution[160] = {0};
};

// Validate shape, mandatory licensing metadata, header checksum and complete
// payload SHA-256. External SD files and downloaded files are both untrusted.
bool validate(const char* path, Metadata* metadata = nullptr);

// Validate the installed pack. If it is damaged, recover a previously rotated
// backup when possible. A user can install content without networking simply
// by copying the .pdl file to PACK_PATH.
bool ensureAvailable(Metadata* metadata = nullptr);

// Atomically replace the active pack with an already-downloaded and validated
// candidate at candidatePath. On failure the prior pack is restored.
bool install(const char* candidatePath, Metadata* metadata = nullptr);

// Random access to one lesson. The returned record is defensively terminated.
bool readRecord(uint32_t index, Record& record);

}  // namespace LearningPack
}  // namespace PocketDaily
