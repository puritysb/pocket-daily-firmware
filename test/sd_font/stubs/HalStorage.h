#pragma once
#include "../../stubs/HalStorage.h"

namespace FakeFontIO {
inline size_t reads = 0;
inline size_t seeks = 0;
inline size_t liveFiles = 0;
inline size_t failSeek = SIZE_MAX;
inline unsigned failOpenAttempt = 0;
}  // namespace FakeFontIO

// Font-specific adapter over the shared fake SD; production uses locked HAL.
class FontFile : public HalFile {
 public:
  FontFile() = default;
  explicit FontFile(HalFile file) : HalFile(std::move(file)), tracked(static_cast<bool>(*this)) {
    if (tracked) ++FakeFontIO::liveFiles;
  }
  FontFile(FontFile&& other) noexcept : HalFile(std::move(other)), tracked(other.tracked) { other.tracked = false; }
  FontFile& operator=(FontFile&& other) noexcept {
    close();
    HalFile::operator=(std::move(other));
    tracked = other.tracked;
    other.tracked = false;
    return *this;
  }
  ~FontFile() { close(); }
  bool seekSet(size_t offset) {
    ++FakeFontIO::seeks;
    return offset != FakeFontIO::failSeek && seek(offset);
  }
  int read(void* output, size_t length) {
    ++FakeFontIO::reads;
    return HalFile::read(output, length);
  }
  void close() {
    if (tracked) --FakeFontIO::liveFiles;
    tracked = false;
    HalFile::close();
  }

 private:
  bool tracked = false;
};
class FontStorage {
 public:
  unsigned readOpens = 0;  // every open attempt; tests bound SD traffic with it
  bool openFileForRead(const char*, const char* path, FontFile& file) {
    ++readOpens;
    if (readOpens == FakeFontIO::failOpenAttempt) return false;
    file = FontFile(HalStorage::getInstance().open(path, O_RDONLY));
    return static_cast<bool>(file);
  }
};
inline FontStorage fontStorage;
#undef Storage
#define Storage fontStorage
#define HalFile FontFile
