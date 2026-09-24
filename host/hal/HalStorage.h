#pragma once

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace PocketUIHost {
// Borrowed immutable assets. The owner must keep names and bytes alive until
// every scope/file/font using them has finished. No OS paths are opened here.
struct Asset {
  std::string_view name;
  std::span<const uint8_t> bytes;
};

// The firmware font loader reopens a path during rasterization. Bind the
// calling host context for the duration of each operation, not globally for
// the context lifetime. Separate contexts can render on separate threads.
class AssetScope {
 public:
  explicit AssetScope(std::span<const Asset> assets) : previous_(current_) { current_ = assets; }
  ~AssetScope() { current_ = previous_; }
  AssetScope(const AssetScope&) = delete;
  AssetScope& operator=(const AssetScope&) = delete;
  static const Asset* find(const char* name) {
    if (!name) return nullptr;
    const Asset* found = nullptr;
    for (const auto& asset : current_) {
      if (asset.name != name) continue;
      if (found) return nullptr;  // Ambiguous names fail closed.
      found = &asset;
    }
    return found;
  }

 private:
  inline static thread_local std::span<const Asset> current_;
  std::span<const Asset> previous_;
};
}  // namespace PocketUIHost

// Read-only HAL for the host renderer. A handle snapshots its asset view, so a
// nested context cannot redirect already-open reads. No copying/allocation of
// font-sized assets and no writable fake-SD state is involved.
class HalFile {
 public:
  HalFile() = default;
  explicit HalFile(std::span<const uint8_t> bytes) : bytes_(bytes), open_(true) {}
  explicit operator bool() const { return open_; }
  size_t size() const { return open_ ? bytes_.size() : 0; }
  bool seek(size_t offset) {
    if (!open_ || offset > bytes_.size()) return false;
    position_ = offset;
    return true;
  }
  bool seekSet(size_t offset) { return seek(offset); }
  bool seekCur(size_t count) {
    if (!open_ || count > bytes_.size() - position_) return false;
    return seek(position_ + count);
  }
  int read(void* output, size_t count) {
    if (!open_ || (!output && count)) return -1;
    const size_t length = std::min({count, bytes_.size() - position_, size_t(INT_MAX)});
    if (length) std::memcpy(output, bytes_.data() + position_, length);
    position_ += length;
    return static_cast<int>(length);
  }
  int read() {
    uint8_t byte;
    return read(&byte, 1) == 1 ? byte : -1;
  }
  void close() { *this = HalFile(); }

 private:
  std::span<const uint8_t> bytes_;
  size_t position_ = 0;
  bool open_ = false;
};

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage storage;
    return storage;
  }
  bool openFileForRead(const char*, const char* path, HalFile& file) const {
    file.close();
    const auto* asset = PocketUIHost::AssetScope::find(path);
    if (!asset) return false;
    file = HalFile(asset->bytes);
    return true;
  }
};
#define Storage HalStorage::getInstance()
