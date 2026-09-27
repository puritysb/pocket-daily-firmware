#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace Articles {
inline constexpr size_t METADATA_BYTES = 523;
inline constexpr char DIRECTORY[] = "/Articles";
inline constexpr char ENTRY[] = "META-INF/pocket-article.bin";

inline bool isFilename(std::string_view name) {
  if (name.size() != 52 || name.substr(0, 11) != "pd-article-" || name.substr(47) != ".epub") return false;
  const auto id = name.substr(11, 36);
  for (size_t i = 0; i < id.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (id[i] != '-') return false;
    } else if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) {
      return false;
    }
  }
  return true;
}
inline bool isPath(std::string_view path) { return path.substr(0, 10) == "/Articles/" && isFilename(path.substr(10)); }
inline uint32_t little32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline uint32_t crc32(const uint8_t* p, size_t size) {
  uint32_t crc = 0xffffffff;
  for (size_t i = 0; i < size; ++i) {
    crc ^= p[i];
    for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}
inline bool validText(const uint8_t* bytes, size_t capacity) {
  const auto* end = static_cast<const uint8_t*>(memchr(bytes, 0, capacity));
  if (!end || end == bytes) return false;
  // UTF-8, including overlong/surrogate/range rejection. Trailing bytes must be padding.
  for (const uint8_t* p = bytes; p < end;) {
    uint32_t cp = *p++;
    if (cp < 0x20 || cp == 0x7f) return false;
    if (cp < 0x80) continue;
    unsigned count = 0;
    uint32_t minimum = 0;
    if (cp >= 0xc2 && cp <= 0xdf) {
      count = 1;
      minimum = 0x80;
      cp &= 0x1f;
    } else if (cp >= 0xe0 && cp <= 0xef) {
      count = 2;
      minimum = 0x800;
      cp &= 0x0f;
    } else if (cp >= 0xf0 && cp <= 0xf4) {
      count = 3;
      minimum = 0x10000;
      cp &= 7;
    } else
      return false;
    if (size_t(end - p) < count) return false;
    while (count--) {
      if ((*p & 0xc0) != 0x80) return false;
      cp = (cp << 6) | (*p++ & 0x3f);
    }
    if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
  }
  for (const uint8_t* p = end; p < bytes + capacity; ++p)
    if (*p != 0) return false;
  return true;
}
struct Metadata {
  uint8_t bytes[METADATA_BYTES]{};
  const char* title() const { return reinterpret_cast<const char*>(bytes + 12); }
  const char* source() const { return reinterpret_cast<const char*>(bytes + 269); }
  uint64_t savedAt() const { return uint64_t(little32(bytes + 4)) | uint64_t(little32(bytes + 8)) << 32; }
  bool valid() const {
    return memcmp(bytes, "PDA1", 4) == 0 && savedAt() < 253402300800ULL && validText(bytes + 12, 257) &&
           validText(bytes + 269, 254);
  }
};
}  // namespace Articles
