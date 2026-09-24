#pragma once

#include <cstdint>
#include <cstring>

#include "UiPack.h"

namespace PocketDaily::LiveStudio::PackState {
inline constexpr size_t BYTES = 64;
struct Record {
  uint32_t generation = 0;
  char name[25]{};
  char version[17]{};
};

inline bool validName(const char* name) {
  if (!name || !*name || strlen(name) > 24) return false;
  for (const char* p = name; *p; ++p) {
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_'))
      return false;
  }
  return true;
}

inline bool validVersion(const char* version) {
  if (!version || !*version || strlen(version) > 16) return false;
  for (const char* p = version; *p; ++p) {
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' ||
          *p == '.'))
      return false;
  }
  return true;
}

inline uint32_t u32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline void put32(uint8_t* p, uint32_t n) {
  for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(n >> (8 * i));
}
inline bool encode(const Record& record, uint8_t (&bytes)[BYTES]) {
  if (record.generation == 0 || !memchr(record.name, 0, sizeof(record.name)) ||
      !memchr(record.version, 0, sizeof(record.version)))
    return false;
  if (*record.name ? (!validName(record.name) || !validVersion(record.version)) : *record.version != 0) return false;
  memset(bytes, 0, BYTES);
  memcpy(bytes, "PDS1", 4);
  put32(bytes + 4, record.generation);
  memcpy(bytes + 8, record.name, strlen(record.name));
  memcpy(bytes + 33, record.version, strlen(record.version));
  put32(bytes + 60, uiPackCrc32(bytes, 60));
  return true;
}
inline bool decode(const uint8_t* bytes, size_t size, Record& record) {
  if (size != BYTES || memcmp(bytes, "PDS1", 4) || u32(bytes + 60) != uiPackCrc32(bytes, 60)) return false;
  Record parsed{};
  parsed.generation = u32(bytes + 4);
  memcpy(parsed.name, bytes + 8, sizeof(parsed.name));
  memcpy(parsed.version, bytes + 33, sizeof(parsed.version));
  uint8_t canonical[BYTES];
  if (!encode(parsed, canonical) || memcmp(canonical, bytes, BYTES)) return false;
  record = parsed;
  return true;
}
// Never overwrite the latest valid slot; equal generations prefer slot 0.
inline int latest(bool valid0, uint32_t generation0, bool valid1, uint32_t generation1) {
  if (!valid0 && !valid1) return -1;
  return valid1 && (!valid0 || generation1 > generation0) ? 1 : 0;
}
}  // namespace PocketDaily::LiveStudio::PackState
