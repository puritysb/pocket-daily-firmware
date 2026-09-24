#include "ContentManifest.h"

#include <cstring>

#include "ContentChecksum.h"

namespace PocketDaily::Content {
namespace {
uint16_t u16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
uint32_t u32(const uint8_t* p) { return uint32_t(u16(p)) | (uint32_t(u16(p + 2)) << 16); }
bool validPath(const char* path, const char* suffix) {
  const size_t length = strnlen(path, 64);
  const size_t suffixLength = strlen(suffix);
  if (length == 64 || length <= suffixLength || strcmp(path + length - suffixLength, suffix) != 0) return false;
  for (size_t i = 0; i < length - suffixLength; ++i) {
    const char c = path[i];
    const bool alphanumeric = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    if (!alphanumeric && (i == 0 || (c != '-' && c != '_'))) return false;
  }
  for (size_t i = length + 1; i < 64; ++i) {
    if (path[i] != 0) return false;
  }
  return true;
}
}  // namespace

bool validContentFileName(const char (&path)[64], FileKind kind) {
  switch (kind) {
    case FileKind::Card:
      return validPath(path, ".card");
    case FileKind::MonoImage:
      return validPath(path, ".pbm");
  }
  return false;
}

bool readManifestEntry(const ManifestSource& source, uint16_t index, ManifestEntry& entry) {
  entry = {};
  const size_t offset = MANIFEST_HEADER_BYTES + size_t(index) * MANIFEST_ENTRY_BYTES;
  if (!source.read || index >= MAX_FILES || source.size < offset + MANIFEST_ENTRY_BYTES + 4) return false;
  uint8_t bytes[MANIFEST_ENTRY_BYTES];
  if (!source.read(source.context, offset, bytes, sizeof(bytes)) || bytes[1] || bytes[2] || bytes[3]) return false;
  if (bytes[0] != static_cast<uint8_t>(FileKind::Card) && bytes[0] != static_cast<uint8_t>(FileKind::MonoImage))
    return false;
  const auto kind = static_cast<FileKind>(bytes[0]);
  if (!validPath(reinterpret_cast<const char*>(bytes + 40), kind == FileKind::Card ? ".card" : ".pbm")) return false;
  entry.kind = kind;
  entry.bytes = u32(bytes + 4);
  memcpy(entry.sha256, bytes + 8, sizeof(entry.sha256));
  memcpy(entry.path, bytes + 40, sizeof(entry.path));
  return true;
}

ManifestResult validateManifest(const ManifestSource& source, uint16_t supportedCapabilities, ManifestInfo& info) {
  info = {};
  if (!source.read || source.size < MANIFEST_HEADER_BYTES + 4 ||
      source.size > MANIFEST_HEADER_BYTES + MANIFEST_ENTRY_BYTES * MAX_FILES + 4)
    return ManifestResult::Shape;
  uint8_t header[MANIFEST_HEADER_BYTES];
  if (!source.read(source.context, 0, header, sizeof(header))) return ManifestResult::ReadFailed;
  if (memcmp(header, "PDCM", 4) || u16(header + 4) != 1 || u16(header + 6) != MANIFEST_HEADER_BYTES ||
      u32(header + 12) != source.size)
    return ManifestResult::Shape;
  const uint16_t count = u16(header + 8);
  const uint16_t capabilities = u16(header + 10);
  if (count > MAX_FILES || source.size != MANIFEST_HEADER_BYTES + MANIFEST_ENTRY_BYTES * count + 4)
    return ManifestResult::Shape;
  if (!(capabilities & CAP_CARDS) || (capabilities & ~SUPPORTED_CAPABILITIES) ||
      (capabilities & supportedCapabilities) != capabilities)
    return ManifestResult::Unsupported;
  uint32_t crc = contentCrcUpdate(0xFFFFFFFFu, header, sizeof(header));
  uint32_t total = 0;
  uint16_t needed = CAP_CARDS;
  uint8_t cards = 0;
  char previous[64]{};
  uint8_t entry[MANIFEST_ENTRY_BYTES];
  for (uint16_t i = 0; i < count; ++i) {
    if (!source.read(source.context, MANIFEST_HEADER_BYTES + i * MANIFEST_ENTRY_BYTES, entry, sizeof(entry)))
      return ManifestResult::ReadFailed;
    crc = contentCrcUpdate(crc, entry, sizeof(entry));
    if (entry[1] || entry[2] || entry[3]) return ManifestResult::Entry;
    const char* path = reinterpret_cast<const char*>(entry + 40);
    const uint32_t bytes = u32(entry + 4);
    uint32_t limit = 0;
    if (entry[0] != static_cast<uint8_t>(FileKind::Card) && entry[0] != static_cast<uint8_t>(FileKind::MonoImage))
      return ManifestResult::Unsupported;
    switch (static_cast<FileKind>(entry[0])) {
      case FileKind::Card:
        if (++cards > 3 || !validPath(path, ".card")) return ManifestResult::Entry;
        limit = 16 * 1024;
        break;
      case FileKind::MonoImage:
        if (!validPath(path, ".pbm")) return ManifestResult::Entry;
        needed |= CAP_IMAGES;
        limit = 64 * 1024;
        break;
    }
    if (!bytes || bytes > limit || bytes > MAX_CONTENT_BYTES - total) return ManifestResult::TooLarge;
    total += bytes;
    if (i && strcmp(previous, path) >= 0) return ManifestResult::Order;
    memcpy(previous, path, sizeof(previous));
  }
  // Layout is inside card bytes, checked by full revision verification.
  if (needed != (capabilities & ~CAP_CARD_LAYOUT)) return ManifestResult::Unsupported;
  uint8_t trailer[4];
  if (!source.read(source.context, source.size - sizeof(trailer), trailer, sizeof(trailer)))
    return ManifestResult::ReadFailed;
  if (u32(trailer) != (crc ^ 0xFFFFFFFFu)) return ManifestResult::Checksum;
  info = {count, capabilities, total};
  return ManifestResult::Ok;
}
}  // namespace PocketDaily::Content
