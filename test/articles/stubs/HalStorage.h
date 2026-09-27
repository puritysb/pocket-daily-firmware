#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>
inline constexpr int O_RDONLY = 0, O_WRONLY = 1, O_CREAT = 2, O_TRUNC = 4;
namespace ArticleSD {
inline std::map<std::string, std::vector<uint8_t>> files;
inline bool failWrites = false;
}  // namespace ArticleSD
class HalFile {
  std::vector<uint8_t>* bytes = nullptr;
  size_t offset = 0;

 public:
  HalFile() = default;
  explicit HalFile(std::vector<uint8_t>* value) : bytes(value) {}
  explicit operator bool() const { return bytes != nullptr; }
  int read(void* output, size_t length) {
    if (!bytes) return 0;
    const size_t count = std::min(length, bytes->size() - offset);
    memcpy(output, bytes->data() + offset, count);
    offset += count;
    return int(count);
  }
  size_t write(uint8_t byte) {
    if (!bytes || ArticleSD::failWrites) return 0;
    bytes->push_back(byte);
    return 1;
  }
  void flush() {}
};
struct ArticleStorageStub {
  HalFile open(const char* path, int flags = O_RDONLY) {
    if (flags & O_CREAT) {
      if (ArticleSD::failWrites) return {};
      auto& data = ArticleSD::files[path];
      data.clear();
      return HalFile(&data);
    }
    auto found = ArticleSD::files.find(path);
    return found == ArticleSD::files.end() ? HalFile() : HalFile(&found->second);
  }
  bool exists(const char* path) { return ArticleSD::files.count(path) != 0; }
  bool mkdir(const char*) { return !ArticleSD::failWrites; }
};
inline ArticleStorageStub Storage;
