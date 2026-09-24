#pragma once

#include <cstddef>
#include <cstdint>

namespace PocketDaily::Content {
inline constexpr size_t MANIFEST_HEADER_BYTES = 16;
inline constexpr size_t MANIFEST_ENTRY_BYTES = 104;
inline constexpr uint16_t MAX_FILES = 16;
inline constexpr uint32_t MAX_CONTENT_BYTES = 256 * 1024;
inline constexpr uint16_t CAP_CARDS = 1;
inline constexpr uint16_t CAP_IMAGES = 2;
inline constexpr uint16_t CAP_CARD_LAYOUT = 4;
inline constexpr uint16_t SUPPORTED_CAPABILITIES = CAP_CARDS | CAP_IMAGES | CAP_CARD_LAYOUT;

enum class FileKind : uint8_t { Card = 1, MonoImage = 2 };
enum class ManifestResult { Ok, ReadFailed, Shape, Unsupported, Entry, Order, TooLarge, Checksum };

struct ManifestSource {
  void* context;
  size_t size;
  bool (*read)(void* context, size_t offset, uint8_t* output, size_t bytes);
};
struct ManifestInfo {
  uint16_t fileCount = 0;
  uint16_t requiredCapabilities = 0;
  uint32_t contentBytes = 0;
};
struct ManifestEntry {
  FileKind kind = FileKind::Card;
  uint32_t bytes = 0;
  uint8_t sha256[32]{};
  char path[64]{};
};
// Read one entry after validateManifest succeeds; source remains immutable.
bool readManifestEntry(const ManifestSource& source, uint16_t index, ManifestEntry& entry);

// Filename occupies a fixed 64-byte NUL-terminated, zero-padded field.
bool validContentFileName(const char (&path)[64], FileKind kind);

// Structural validation only. Does not check referenced files, activate content
// or authenticate a sender. Source must remain immutable for the entire call.
// Allocation-free; on failure info is empty. Contract: docs/content-manifest-v1.md.
ManifestResult validateManifest(const ManifestSource& source, uint16_t supportedCapabilities, ManifestInfo& info);
}  // namespace PocketDaily::Content
