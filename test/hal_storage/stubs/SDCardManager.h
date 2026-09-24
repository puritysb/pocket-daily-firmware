#pragma once
#include <Print.h>
#include <common/FsApiConstants.h>
#include <freertos/semphr.h>

#include <cstring>
#include <utility>
#include <vector>

namespace FakeSDK {
inline unsigned opens = 0, closes = 0, nexts = 0, writes = 0, unlocked = 0;
inline bool failOpen = false;
inline bool directory = true;
inline uint8_t filesystemType = 32;
inline bool errorAfterRead = false;
inline uint8_t readError = 0, recordByte = 7;
inline int readCount = -2;
inline unsigned reads = 0;
inline void checkLock() {
  if (lockDepth == 0) ++unlocked;
}
inline void reset() {
  opens = closes = nexts = writes = unlocked = 0;
  failOpen = false;
  directory = true;
  filesystemType = 32;
  errorAfterRead = false;
  readError = 0;
  recordByte = 7;
  readCount = -2;
  reads = 0;
}
}  // namespace FakeSDK

class FsFile {
 public:
  FsFile() = default;
  explicit FsFile(bool open) : opened(open) {}
  FsFile(const FsFile&) = delete;
  FsFile& operator=(const FsFile&) = delete;
  FsFile(FsFile&& other) : opened(std::exchange(other.opened, false)) {}
  FsFile& operator=(FsFile&& other) {
    close();
    opened = std::exchange(other.opened, false);
    return *this;
  }
  ~FsFile() { close(); }
  bool close() {
    if (opened) {
      FakeSDK::checkLock();
      ++FakeSDK::closes;
    }
    opened = false;
    return true;
  }
  bool isOpen() const { return opened; }
  bool isDirectory() const { return opened && FakeSDK::directory; }
  uint8_t getError() const {
    FakeSDK::checkLock();
    return FakeSDK::readError;
  }
  size_t size() const { return opened ? 7 : 0; }
  size_t fileSize() const { return size(); }
  uint64_t fileSize64() const { return size(); }
  void flush() { FakeSDK::checkLock(); }
  size_t getName(char* out, size_t len) {
    FakeSDK::checkLock();
    if (len) out[0] = 0;
    return 0;
  }
  bool seekSet(uint64_t pos) {
    FakeSDK::checkLock();
    cursor = pos;
    return opened;
  }
  bool seekCur(int64_t) {
    FakeSDK::checkLock();
    return opened;
  }
  bool preAllocate(size_t) {
    FakeSDK::checkLock();
    return opened;
  }
  int available() const {
    FakeSDK::checkLock();
    return opened ? 7 : 0;
  }
  size_t position() const {
    FakeSDK::checkLock();
    return cursor;
  }
  int read(void* out, size_t count) {
    FakeSDK::checkLock();
    if (!opened) return -1;
    ++FakeSDK::reads;
    if (FakeSDK::errorAfterRead) FakeSDK::readError = 1;
    const int result = FakeSDK::readCount == -2 ? static_cast<int>(count) : FakeSDK::readCount;
    if (result > 0) {
      memset(out, FakeSDK::recordByte, result);
      cursor += result;
    }
    return result;
  }
  int read() {
    FakeSDK::checkLock();
    return opened ? 7 : -1;
  }
  size_t write(const void*, size_t count) {
    FakeSDK::checkLock();
    ++FakeSDK::writes;
    return opened ? count : 0;
  }
  size_t write(uint8_t byte) { return write(&byte, 1); }
  bool rename(const char*) {
    FakeSDK::checkLock();
    return opened;
  }
  void rewindDirectory() { FakeSDK::checkLock(); }
  FsFile openNextFile() {
    FakeSDK::checkLock();
    ++FakeSDK::nexts;
    return FsFile(opened);
  }

 private:
  bool opened = false;
  size_t cursor = 0;
};

class SDCardManager {
 public:
  static SDCardManager& getInstance() {
    static SDCardManager manager;
    return manager;
  }
  bool begin() { return true; }
  bool ready() const { return true; }
  uint8_t filesystemType() const {
    FakeSDK::checkLock();
    return FakeSDK::filesystemType;
  }
  std::vector<String> listFiles(const char*, int) { return {}; }
  String readFile(const char*) { return {}; }
  bool readFileToStream(const char*, Print&, size_t) { return true; }
  size_t readFileToBuffer(const char*, char*, size_t, size_t) { return 0; }
  bool writeFile(const char*, const String&) { return true; }
  bool ensureDirectoryExists(const char*) { return true; }
  bool mkdir(const char*, bool) { return true; }
  bool exists(const char*) { return true; }
  bool remove(const char*) { return true; }
  bool rename(const char*, const char*) { return true; }
  bool rmdir(const char*) { return true; }
  bool removeDir(const char*) { return true; }
  FsFile open(const char*, oflag_t) {
    FakeSDK::checkLock();
    ++FakeSDK::opens;
    return FsFile(!FakeSDK::failOpen);
  }
  bool openFileForRead(const char*, const char* path, FsFile& out) {
    out = open(path, 0);
    return out.isOpen();
  }
  bool openFileForWrite(const char*, const char* path, FsFile& out) {
    out = open(path, 1);
    return out.isOpen();
  }
};
