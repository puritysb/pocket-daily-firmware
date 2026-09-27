#pragma once
// Host test HAL: files live under a temporary directory (TestFs::root).
#include <HalIoCounters.h>
#include <Print.h>
#include <WString.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#ifndef O_RDONLY
#define O_RDONLY 0
#endif
#ifndef O_WRITE
#define O_WRITE 1
#endif
#ifndef O_CREAT
#define O_CREAT 0x200
#endif
#ifndef O_TRUNC
#define O_TRUNC 0x400
#endif

struct TestFs {
  inline static std::string root;
  static std::string map(const char* p) { return root + p; }
};

class HalFile : public Print {
 public:
  HalFile() = default;
  ~HalFile() override { close(); }
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;
  HalFile(HalFile&& o) noexcept : fp_(o.fp_) { o.fp_ = nullptr; }
  HalFile& operator=(HalFile&& o) noexcept {
    if (this != &o) {
      close();
      fp_ = o.fp_;
      o.fp_ = nullptr;
    }
    return *this;
  }
  bool openPath(const std::string& path, const char* mode) {
    close();
    fp_ = std::fopen(path.c_str(), mode);
    if (fp_) halIoCounters.opens++;
    return fp_ != nullptr;
  }
  explicit operator bool() const { return fp_ != nullptr; }
  bool isOpen() const { return fp_ != nullptr; }
  bool isDirectory() const { return false; }
  size_t size() const {
    if (!fp_) return 0;
    const long p = std::ftell(fp_);
    std::fseek(fp_, 0, SEEK_END);
    const long s = std::ftell(fp_);
    std::fseek(fp_, p, SEEK_SET);
    return s < 0 ? 0 : static_cast<size_t>(s);
  }
  size_t fileSize() const { return size(); }
  size_t position() const { return fp_ ? static_cast<size_t>(std::ftell(fp_)) : 0; }
  int available() const { return fp_ ? static_cast<int>(size() - position()) : 0; }
  bool seek(size_t off) { return fp_ && std::fseek(fp_, static_cast<long>(off), SEEK_SET) == 0; }
  bool seekSet(size_t off) { return seek(off); }
  bool seekCur(int64_t n) { return fp_ && std::fseek(fp_, static_cast<long>(n), SEEK_CUR) == 0; }
  int read(void* out, size_t n) {
    if (!fp_) return -1;
    const int got = static_cast<int>(std::fread(out, 1, n, fp_));
    halIoCounters.readCalls++;
    if (got > 0) halIoCounters.readBytes += static_cast<uint32_t>(got);
    return got;
  }
  int read() {
    uint8_t b;
    return read(&b, 1) == 1 ? b : -1;
  }
  size_t write(const void* in, size_t n) { return fp_ ? std::fwrite(in, 1, n, fp_) : 0; }
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t* b, size_t n) override { return write(static_cast<const void*>(b), n); }
  void flush() {
    if (fp_) std::fflush(fp_);
  }
  bool close() {
    if (fp_) std::fclose(fp_);
    fp_ = nullptr;
    return true;
  }

 private:
  mutable FILE* fp_ = nullptr;
};

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage s;
    return s;
  }
  bool openFileForRead(const char*, const char* path, HalFile& f) { return f.openPath(TestFs::map(path), "rb"); }
  bool openFileForRead(const char* t, const std::string& path, HalFile& f) {
    return openFileForRead(t, path.c_str(), f);
  }
  bool openFileForWrite(const char*, const char* path, HalFile& f) { return f.openPath(TestFs::map(path), "wb+"); }
  bool openFileForWrite(const char* t, const std::string& path, HalFile& f) {
    return openFileForWrite(t, path.c_str(), f);
  }
  HalFile open(const char* path, int flags = O_RDONLY) {
    HalFile f;
    f.openPath(TestFs::map(path), (flags & O_WRITE) ? "wb+" : "rb");
    return f;
  }
  bool exists(const char* p) {
    struct stat st;
    return ::stat(TestFs::map(p).c_str(), &st) == 0;
  }
  bool remove(const char* p) { return ::unlink(TestFs::map(p).c_str()) == 0; }
  bool mkdir(const char* p, bool = true) {
    ::mkdir(TestFs::map(p).c_str(), 0755);
    return true;
  }
  bool rename(const char* a, const char* b) { return std::rename(TestFs::map(a).c_str(), TestFs::map(b).c_str()) == 0; }
  bool removeDir(const char* p) {
    const std::string cmd = "rm -rf '" + TestFs::map(p) + "'";
    return std::system(cmd.c_str()) == 0;
  }
  bool rmdir(const char* p) { return removeDir(p); }
  bool ensureDirectoryExists(const char* p) { return mkdir(p); }
};
#define Storage HalStorage::getInstance()
