#pragma once
#include <HalStorage.h>
#include <Logging.h>

#include <string>

#include "ArticleFormat.h"

namespace Articles {
// The app writes mimetype first and this bounded, stored metadata entry second.
// Read only these local headers: no EPUB DOM, decompressor or book cache is needed for a list row.
// HalFile::read writes through metadata.bytes; cppcheck cannot infer the SDK HAL out-buffer contract.
// cppcheck-suppress constParameterReference
inline bool readMetadata(const char* path, Metadata& metadata) {
  auto file = Storage.open(path);
  if (!file) return false;
  uint8_t header[30];
  char name[27]{};
  if (file.read(header, 30) != 30 || little32(header) != 0x04034b50 || header[6] || header[7] || header[8] ||
      header[9] || little32(header + 18) != 20 || little32(header + 22) != 20 || header[26] != 8 || header[27] ||
      header[28] || header[29])
    return false;
  if (file.read(name, 8) != 8 || memcmp(name, "mimetype", 8) != 0) return false;
  if (file.read(name, 20) != 20 || memcmp(name, "application/epub+zip", 20) != 0) return false;
  if (file.read(header, 30) != 30 || little32(header) != 0x04034b50 || header[6] || header[7] || header[8] ||
      header[9] || little32(header + 18) != METADATA_BYTES || little32(header + 22) != METADATA_BYTES ||
      header[26] != sizeof(ENTRY) - 1 || header[27] || header[28] || header[29])
    return false;
  if (file.read(name, sizeof(ENTRY) - 1) != sizeof(ENTRY) - 1 || memcmp(name, ENTRY, sizeof(ENTRY) - 1) != 0)
    return false;
  return file.read(metadata.bytes, METADATA_BYTES) == METADATA_BYTES && metadata.valid() &&
         crc32(metadata.bytes, METADATA_BYTES) == little32(header + 14);
}
inline std::string donePath(const std::string& path) {
  return "/.crosspoint/articles/" + path.substr(21, 36) + ".done";
}
inline bool isRead(const std::string& path) {
  auto file = Storage.open(donePath(path).c_str());
  uint8_t value = 0;
  return file && file.read(&value, 1) == 1 && value == 1;
}
inline bool markRead(const std::string& path) {
  if (!isPath(path)) return false;
  const auto marker = donePath(path);
  if (isRead(path)) return true;
  if (!Storage.exists("/.crosspoint/articles") && !Storage.mkdir("/.crosspoint/articles")) return false;
  auto file = Storage.open(marker.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
  if (!file) {
    LOG_ERR("ARTICLE", "Cannot save read status");
    return false;
  }
  const bool written = file.write(uint8_t(1)) == 1;
  file.flush();
  return written;
}
}  // namespace Articles
