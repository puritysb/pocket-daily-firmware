#pragma once

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>

inline constexpr int O_RDONLY = 0;
inline constexpr int O_WRITE = 1;
inline constexpr int O_CREAT = 2;
inline constexpr int O_TRUNC = 4;
inline constexpr int O_EXCL = 8;

namespace FakeSD {
inline std::map<std::string, std::vector<uint8_t>> files;
inline size_t writeBudget = std::numeric_limits<size_t>::max();
inline size_t readBudget = std::numeric_limits<size_t>::max();
inline bool available = true;
inline bool failWriteOpen = false;
inline bool corruptWrite = false;
inline bool failRename = false;
inline bool failAfterRename = false;
inline std::string failReadOpen;
inline std::string failRemove;
inline std::vector<std::string> falseMissing;
inline bool directoryReadError = false, unknownFormat = false;
inline bool extraAfterRename = false;
inline unsigned deletedDirectorySlots = 0;
inline void reset() {
  files.clear();
  writeBudget = readBudget = std::numeric_limits<size_t>::max();
  available = true;
  failWriteOpen = corruptWrite = false;
  failRename = failAfterRename = false;
  failReadOpen.clear();
  failRemove.clear();
  falseMissing.clear();
  directoryReadError = unknownFormat = false;
  extraAfterRename = false;
  deletedDirectorySlots = 0;
}
}  // namespace FakeSD

class HalFile {
 public:
  HalFile() = default;
  explicit HalFile(std::vector<uint8_t>* contents) : bytes(contents) {}
  explicit HalFile(std::string directory) : directory(std::move(directory)) {}
  operator bool() const { return bytes != nullptr || !directory.empty(); }
  enum class DirectoryRead { Record, End, Error };
  DirectoryRead readDirectoryRecord(uint8_t (&record)[32]) {
    memset(record, 0, sizeof(record));
    if (directory.empty() || FakeSD::directoryReadError) return DirectoryRead::Error;
    if (directoryIndex < FakeSD::deletedDirectorySlots) {
      ++directoryIndex;
      record[0] = 0xe5;
      return DirectoryRead::Record;
    }
    // Synthetic FAT slots from the fake tree. Production parsing/limits are
    // still real; this fake does not claim to emulate a physical FAT image.
    std::map<std::string, std::pair<bool, size_t>> entries;
    const auto prefix = directory + "/";
    for (const auto& [path, contents] : FakeSD::files) {
      if (!path.starts_with(prefix)) continue;
      const auto child = path.substr(prefix.size());
      const auto slash = child.find('/');
      entries[child.substr(0, slash)] = {slash != std::string::npos, contents.size()};
    }
    const auto index = directoryIndex - FakeSD::deletedDirectorySlots;
    if (index >= entries.size()) return DirectoryRead::End;
    auto entry = entries.begin();
    std::advance(entry, index);
    ++directoryIndex;
    memcpy(record, "FILE    BIN", 11);
    record[11] = entry->second.first ? 0x10 : 0x20;
    const uint32_t size = entry->second.first ? 0 : entry->second.second;
    for (unsigned i = 0; i < 4; ++i) record[28 + i] = size >> (8 * i);
    return DirectoryRead::Record;
  }
  size_t size() const { return bytes ? bytes->size() : 0; }
  bool close() {
    bytes = nullptr;
    directory.clear();
    return true;
  }
  bool seek(size_t offset) {
    if (!bytes || offset > bytes->size()) return false;
    position = offset;
    return true;
  }
  int read(void* output, size_t count) {
    if (!bytes) return -1;
    const auto length = std::min({count, bytes->size() - position, FakeSD::readBudget});
    memcpy(output, bytes->data() + position, length);
    position += length;
    FakeSD::readBudget -= length;
    return static_cast<int>(length);
  }
  bool seekCur(size_t count) {
    if (!bytes || count > bytes->size() - position) return false;
    return seek(position + count);
  }
  int read() {
    uint8_t byte = 0;
    return read(&byte, 1) == 1 ? byte : -1;
  }
  size_t write(const void* input, size_t count) {
    if (!bytes) return 0;
    const auto length = std::min(count, FakeSD::writeBudget);
    bytes->resize(position + length);
    memcpy(bytes->data() + position, input, length);
    if (FakeSD::corruptWrite && length) (*bytes)[position] ^= 1;
    position += length;
    FakeSD::writeBudget -= length;
    return length;
  }

 private:
  std::vector<uint8_t>* bytes = nullptr;
  size_t position = 0;
  std::string directory;
  size_t directoryIndex = 0;
};

class HalStorage {
 public:
  enum class FilesystemFormat { Unavailable, Fat, ExFat };
  FilesystemFormat filesystemFormat() const {
    return FakeSD::available && !FakeSD::unknownFormat ? FilesystemFormat::Fat : FilesystemFormat::Unavailable;
  }
  static HalStorage& getInstance() {
    static HalStorage storage;
    return storage;
  }
  bool ready() const { return FakeSD::available; }
  bool exists(const char* path) {
    if (!ready()) return false;
    if (std::find(FakeSD::falseMissing.begin(), FakeSD::falseMissing.end(), path) != FakeSD::falseMissing.end())
      return false;
    if (FakeSD::files.count(path)) return true;
    const std::string prefix = std::string(path) + "/";
    const auto it = FakeSD::files.lower_bound(prefix);
    return it != FakeSD::files.end() && it->first.starts_with(prefix);
  }
  bool mkdir(const char*) { return ready(); }
  bool remove(const char* path) {
    if (!ready() || FakeSD::failRemove == path) return false;
    return FakeSD::files.erase(path) == 1;
  }
  bool rmdir(const char* path) {
    // Directories are implicit in this fake. Never erase descendants.
    return ready() && !exists(path);
  }
  bool rename(const char* source, const char* target) {
    if (!ready() || FakeSD::failRename || !exists(source) || exists(target)) return false;
    const std::string prefix = std::string(source) + "/";
    auto moved = FakeSD::files;
    for (const auto& [path, bytes] : FakeSD::files) {
      if (path == source || path.starts_with(prefix)) {
        moved.erase(path);
        moved[std::string(target) + path.substr(strlen(source))] = bytes;
      }
    }
    FakeSD::files = std::move(moved);
    if (FakeSD::extraAfterRename) FakeSD::files[std::string(target) + "/unexpected.part"] = {};
    return !FakeSD::failAfterRename;
  }
  HalFile open(const char* path, int flags) {
    if (!ready()) return {};
    if (flags & O_WRITE) {
      if (FakeSD::failWriteOpen) return {};
      if ((flags & O_EXCL) && FakeSD::files.count(path)) return {};
      auto& bytes = FakeSD::files[path];
      if (flags & O_TRUNC) bytes.clear();
      return HalFile(&bytes);
    }
    if (FakeSD::failReadOpen == path) return {};
    if (!FakeSD::files.count(path)) return exists(path) ? HalFile(std::string(path)) : HalFile{};
    return HalFile(&FakeSD::files.at(path));
  }
};
#define Storage HalStorage::getInstance()
