#pragma once

#include <Print.h>
#include <common/FsApiConstants.h>  // for oflag_t
#include <freertos/semphr.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class HalFile;

class HalStorage {
 public:
  HalStorage();
  bool begin();
  bool ready() const;
  enum class FilesystemFormat { Unavailable, Fat, ExFat };
  // Reads mounted SDK metadata under the storage lock; never scans the card.
  FilesystemFormat filesystemFormat() const;
  struct SpaceChunk {
    uint64_t totalBytes = 0, freeBytes = 0;
    uint32_t nextCluster = 0;
    bool supported = false;
  };
  // At most 4096 clusters per request; yields between sectors, never scans on heartbeat.
  bool spaceChunk(uint32_t cluster, SpaceChunk& result, void (*progress)());
  std::vector<String> listFiles(const char* path = "/", int maxFiles = 200);
  // Read the entire file at `path` into a String. Returns empty string on failure.
  String readFile(const char* path);
  // Low-memory helpers:
  // Stream the file contents to a `Print` (e.g. `Serial`, or any `Print`-derived object).
  // Returns true on success, false on failure.
  bool readFileToStream(const char* path, Print& out, size_t chunkSize = 256);
  // Read up to `bufferSize-1` bytes into `buffer`, null-terminating it. Returns bytes read.
  size_t readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes = 0);
  // Write a string to `path` on the SD card. Overwrites existing file.
  // Returns true on success.
  bool writeFile(const char* path, const String& content);
  // Ensure a directory exists, creating it if necessary. Returns true on success.
  bool ensureDirectoryExists(const char* path);

  HalFile open(const char* path, const oflag_t oflag = O_RDONLY);
  bool mkdir(const char* path, const bool pFlag = true);
  bool exists(const char* path);
  bool remove(const char* path);
  bool rename(const char* oldPath, const char* newPath);
  bool rmdir(const char* path);

  bool openFileForRead(const char* moduleName, const char* path, HalFile& file);
  bool openFileForRead(const char* moduleName, const std::string& path, HalFile& file);
  bool openFileForRead(const char* moduleName, const String& path, HalFile& file);
  bool openFileForWrite(const char* moduleName, const char* path, HalFile& file);
  bool openFileForWrite(const char* moduleName, const std::string& path, HalFile& file);
  bool openFileForWrite(const char* moduleName, const String& path, HalFile& file);
  bool removeDir(const char* path);

  static HalStorage& getInstance() { return instance; }

  class StorageLock;  // private class, used internally

 private:
  static HalStorage instance;

  bool initialized = false;
  SemaphoreHandle_t storageMutex = nullptr;
};

#define Storage HalStorage::getInstance()

class HalFile : public Print {
  friend class HalStorage;
  class Impl;
  std::unique_ptr<Impl> impl;
  // setWriteBuffer() state; moves with the handle (the moved-from handle keeps none).
  struct WriteBuffer {
    uint8_t* data = nullptr;
    size_t capacity = 0;
    size_t pending = 0;
    WriteBuffer() = default;
    WriteBuffer(const WriteBuffer&) = delete;
    WriteBuffer& operator=(const WriteBuffer&) = delete;
    WriteBuffer(WriteBuffer&& other) noexcept : data(other.data), capacity(other.capacity), pending(other.pending) {
      other.data = nullptr;
      other.capacity = other.pending = 0;
    }
    WriteBuffer& operator=(WriteBuffer&& other) noexcept {
      data = other.data;
      capacity = other.capacity;
      pending = other.pending;
      other.data = nullptr;
      other.capacity = other.pending = 0;
      return *this;
    }
  };
  WriteBuffer wbuf_;
  // Writes the pending buffered bytes; false when the SD write came up short.
  bool drainWriteBuffer();
  static std::unique_ptr<Impl> allocateImpl();
  explicit HalFile(std::unique_ptr<Impl> impl);

 public:
  // Empty/OOM handles are safe to close and inspect. Reads fail with -1,
  // writes return zero, and seek/rename/preallocation return false.
  HalFile();
  ~HalFile();
  HalFile(HalFile&&) noexcept;
  HalFile& operator=(HalFile&&) noexcept;
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  // Write-behind buffer (caller-owned, `capacity` bytes): write() collects small writes and
  // hands the SD layer whole chunks, so a stream of 2-4 byte fields becomes one locked,
  // sector-spanning transfer per chunk. Every other operation (seek, read, size, flush, close,
  // rename, ...) first writes the pending bytes; position() includes them. The buffer must
  // outlive its use: pass nullptr (which writes pending bytes) or close() before freeing it.
  void setWriteBuffer(uint8_t* buffer, size_t capacity);
  void flush();
  size_t getName(char* name, size_t len);
  size_t size();
  size_t fileSize();
  uint64_t fileSize64();
  bool seek(size_t pos);
  bool seek64(uint64_t pos);
  bool seekCur(int64_t offset);
  bool seekSet(size_t offset);
  // Reserve `length` contiguous bytes for an empty, writable file so a large
  // sequential transfer allocates clusters once instead of on every write.
  // Returns false (and leaves the file usable) when the card cannot provide
  // the contiguous span. Reported size() becomes `length` immediately.
  bool preAllocate(size_t length);
  int available() const;
  size_t position() const;
  int read(void* buf, size_t count);
  int read();  // read a single byte
  size_t write(const void* buf, size_t count);
  size_t write(uint8_t b) override;
  bool rename(const char* newPath);
  bool isDirectory() const;
  void rewindDirectory();
  bool close();
  HalFile openNextFile();
  enum class DirectoryRead { Record, End, Error };
  DirectoryRead openNextEntry(HalFile& entry);
  // One raw 32-byte FAT/exFAT directory slot, including deleted/LFN slots.
  // Does not decode names or prove directory semantic validity. Use a dedicated
  // directory handle at a 32-byte boundary and stop on End/Error. Callers bound
  // the total number of slots and yield between calls; never mix with openNextFile.
  // End means a clean physical EOF or a zero entry marker, not a complete valid
  // inventory. Partial records, sticky SDK errors and invalid handles are Error.
  // record is cleared on End/Error. No additional handle/buffer allocation.
  DirectoryRead readDirectoryRecord(uint8_t (&record)[32]);
  bool isOpen() const;
  operator bool() const;
};

// Downstream code must use Storage instead of SdMan
#ifdef SdMan
#undef SdMan
#endif
