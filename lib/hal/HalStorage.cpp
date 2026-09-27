#include "HalStorage.h"

#include <FS.h>  // need to be included before SdFat.h for compatibility with FS.h's File class
#include <HalIoCounters.h>
#include <Logging.h>
#include <Memory.h>
#include <SDCardManager.h>

#include <algorithm>
#include <cassert>
#include <cstring>

#define SDCard SDCardManager::getInstance()

HalStorage HalStorage::instance;

HalStorage::HalStorage() {
  // Recursive so the same task can re-enter StorageLock without self-deadlock.
  // openFileForRead/Write take the lock and then assign to a HalFile&
  // out-param; if that out-param already held an Impl, its destructor takes
  // the lock again to close the prior FsFile under serialization (see
  // HalFile::Impl::~Impl below). Priority inheritance still applies to
  // recursive mutexes.
  storageMutex = xSemaphoreCreateRecursiveMutex();
  assert(storageMutex != nullptr);
}

// begin() and ready() are only called from setup, no need to acquire mutex for them

bool HalStorage::begin() { return SDCard.begin(); }

bool HalStorage::ready() const { return SDCard.ready(); }

// For the rest of the methods, we acquire the mutex to ensure thread safety

class HalStorage::StorageLock {
 public:
  StorageLock() { xSemaphoreTakeRecursive(HalStorage::getInstance().storageMutex, portMAX_DELAY); }
  ~StorageLock() { xSemaphoreGiveRecursive(HalStorage::getInstance().storageMutex); }
};

HalStorage::FilesystemFormat HalStorage::filesystemFormat() const {
  StorageLock lock;
  switch (SDCard.filesystemType()) {
    case 12:
    case 16:
    case 32:
      return FilesystemFormat::Fat;
    case 64:
      return FilesystemFormat::ExFat;
    default:
      return FilesystemFormat::Unavailable;
  }
}

bool HalStorage::spaceChunk(uint32_t cluster, SpaceChunk& result, void (*progress)()) {
  result = {};
  StorageLock lock;
  if (!SDCard.ready()) return false;
  const uint32_t count = SDCard.volumeClusters(), clusterBytes = SDCard.clusterBytes();
  result.totalBytes = static_cast<uint64_t>(count) * clusterBytes;
  const auto type = SDCard.filesystemType();
  if (type != 16 && type != 32) return true;
  if (!count || !clusterBytes || count > 0x0FFFFFF5U) return false;
  if (cluster == 0) cluster = 2;
  if (cluster < 2 || cluster > count + 2) return false;
  // One temporary 512-byte sector is too large for the task stack; released per request.
  struct Sector {
    uint8_t bytes[512];
  };
  auto sector = makeUniqueNoThrow<Sector>();
  if (!sector) {
    LOG_ERR("STORAGE", "OOM reading space usage");
    return false;
  }
  const uint32_t width = type == 16 ? 2 : 4;
  const uint32_t end = std::min(cluster + 4096U, count + 2);
  uint32_t loaded = UINT32_MAX;
  for (; cluster < end; ++cluster) {
    const uint32_t index = cluster / (512 / width);
    if (loaded != index) {
      if (!SDCard.readAllocationSector(SDCard.fatStart() + index, sector->bytes)) return false;
      loaded = index;
      if (progress) progress();
    }
    const auto* bytes = sector->bytes + (cluster % (512 / width)) * width;
    uint32_t value = bytes[0] | (uint32_t(bytes[1]) << 8);
    if (width == 4) value |= (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3] & 15) << 24);
    if (value == 0) result.freeBytes += clusterBytes;
  }
  result.supported = true;
  result.nextCluster = end < count + 2 ? end : 0;
  return true;
}

#define HAL_STORAGE_WRAPPED_CALL(method, ...) \
  HalStorage::StorageLock lock;               \
  return SDCard.method(__VA_ARGS__);

std::vector<String> HalStorage::listFiles(const char* path, int maxFiles) {
  HAL_STORAGE_WRAPPED_CALL(listFiles, path, maxFiles);
}

String HalStorage::readFile(const char* path) { HAL_STORAGE_WRAPPED_CALL(readFile, path); }

bool HalStorage::readFileToStream(const char* path, Print& out, size_t chunkSize) {
  HAL_STORAGE_WRAPPED_CALL(readFileToStream, path, out, chunkSize);
}

size_t HalStorage::readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes) {
  HAL_STORAGE_WRAPPED_CALL(readFileToBuffer, path, buffer, bufferSize, maxBytes);
}

bool HalStorage::writeFile(const char* path, const String& content) {
  HAL_STORAGE_WRAPPED_CALL(writeFile, path, content);
}

bool HalStorage::ensureDirectoryExists(const char* path) { HAL_STORAGE_WRAPPED_CALL(ensureDirectoryExists, path); }

class HalFile::Impl {
 public:
  // SdFat is not thread-safe; FsFile::close() touches SD/SPI and must run
  // under StorageLock or it races SdSpiCard::m_spiActive across tasks and
  // trips FreeRTOS's xTaskPriorityDisinherit assert. The FsFile member
  // destructor (DESTRUCTOR_CLOSES_FILE=1) will close() again after the lock
  // releases, but close() on an already-closed FsFile is a no-op. See SdFat
  // issue #518 and the HAL note in CLAUDE.md.
  ~Impl() {
    HalStorage::StorageLock lock;
    file.close();
  }
  FsFile file;
};

std::unique_ptr<HalFile::Impl> HalFile::allocateImpl() {
  // Same sizeof(Impl) allocation as before: a movable handle must outlive its
  // opening stack frame. Allocate before any SDK mutation, never a static pool.
  auto allocated = makeUniqueNoThrow<Impl>();
  if (!allocated) LOG_ERR("STORAGE", "OOM allocating %uB file handle", static_cast<unsigned>(sizeof(Impl)));
  return allocated;
}

HalFile::HalFile() = default;
HalFile::HalFile(std::unique_ptr<Impl> impl) : impl(std::move(impl)) {}
HalFile::~HalFile() = default;
HalFile::HalFile(HalFile&&) = default;
HalFile& HalFile::operator=(HalFile&&) = default;

HalFile HalStorage::open(const char* path, const oflag_t oflag) {
  StorageLock lock;  // ensure thread safety for the duration of this function
  auto impl = HalFile::allocateImpl();
  if (!impl) return {};
  impl->file = SDCard.open(path, oflag);
  if (impl->file.isOpen()) halIoCounters.opens++;
  return HalFile(std::move(impl));
}

bool HalStorage::mkdir(const char* path, const bool pFlag) { HAL_STORAGE_WRAPPED_CALL(mkdir, path, pFlag); }

bool HalStorage::exists(const char* path) { HAL_STORAGE_WRAPPED_CALL(exists, path); }

bool HalStorage::remove(const char* path) { HAL_STORAGE_WRAPPED_CALL(remove, path); }
bool HalStorage::rename(const char* oldPath, const char* newPath) {
  HAL_STORAGE_WRAPPED_CALL(rename, oldPath, newPath);
}

bool HalStorage::rmdir(const char* path) { HAL_STORAGE_WRAPPED_CALL(rmdir, path); }

bool HalStorage::openFileForRead(const char* moduleName, const char* path, HalFile& file) {
  StorageLock lock;  // ensure thread safety for the duration of this function
  auto impl = HalFile::allocateImpl();
  if (!impl) {
    file = {};
    return false;
  }
  const bool ok = SDCard.openFileForRead(moduleName, path, impl->file);
  if (ok) halIoCounters.opens++;
  file = HalFile(std::move(impl));
  return ok;
}

bool HalStorage::openFileForRead(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForRead(const char* moduleName, const String& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const char* path, HalFile& file) {
  StorageLock lock;  // ensure thread safety for the duration of this function
  auto impl = HalFile::allocateImpl();
  if (!impl) {
    file = {};
    return false;
  }
  const bool ok = SDCard.openFileForWrite(moduleName, path, impl->file);
  if (ok) halIoCounters.opens++;
  file = HalFile(std::move(impl));
  return ok;
}

bool HalStorage::openFileForWrite(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const String& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::removeDir(const char* path) { HAL_STORAGE_WRAPPED_CALL(removeDir, path); }

// HalFile implementation
// Allow doing file operations while ensuring thread safety via HalStorage's mutex.
// Please keep the list below in sync with the HalFile.h header

#define HAL_FILE_WRAPPED_CALL(method, fallback, ...) \
  HalStorage::StorageLock lock;                      \
  if (!impl) return fallback;                        \
  return impl->file.method(__VA_ARGS__);

#define HAL_FILE_FORWARD_CALL(method, fallback, ...) \
  if (!impl) return fallback;                        \
  return impl->file.method(__VA_ARGS__);

void HalFile::flush() { HAL_FILE_WRAPPED_CALL(flush, , ); }
size_t HalFile::getName(char* name, size_t len) {
  if (!impl && name && len) name[0] = '\0';
  HAL_FILE_WRAPPED_CALL(getName, 0, name, len);
}
size_t HalFile::size() { HAL_FILE_FORWARD_CALL(size, 0, ); }
size_t HalFile::fileSize() { HAL_FILE_FORWARD_CALL(fileSize, 0, ); }
uint64_t HalFile::fileSize64() { HAL_FILE_FORWARD_CALL(fileSize, 0, ); }
bool HalFile::seek(size_t pos) { HAL_FILE_WRAPPED_CALL(seekSet, false, pos); }
bool HalFile::seek64(uint64_t pos) { HAL_FILE_WRAPPED_CALL(seekSet, false, pos); }
bool HalFile::seekCur(int64_t offset) { HAL_FILE_WRAPPED_CALL(seekCur, false, offset); }
bool HalFile::seekSet(size_t offset) { HAL_FILE_WRAPPED_CALL(seekSet, false, offset); }
bool HalFile::preAllocate(size_t length) { HAL_FILE_WRAPPED_CALL(preAllocate, false, length); }
int HalFile::available() const { HAL_FILE_WRAPPED_CALL(available, 0, ); }
size_t HalFile::position() const { HAL_FILE_WRAPPED_CALL(position, 0, ); }
int HalFile::read(void* buf, size_t count) {
  HalStorage::StorageLock lock;
  if (!impl) return -1;
  const int got = impl->file.read(buf, count);
  halIoCounters.readCalls++;
  if (got > 0) halIoCounters.readBytes += static_cast<uint32_t>(got);
  return got;
}
int HalFile::read() {
  HalStorage::StorageLock lock;
  if (!impl) return -1;
  const int value = impl->file.read();
  halIoCounters.readCalls++;
  if (value >= 0) halIoCounters.readBytes++;
  return value;
}
size_t HalFile::write(const void* buf, size_t count) { HAL_FILE_WRAPPED_CALL(write, 0, buf, count); }
size_t HalFile::write(uint8_t b) { HAL_FILE_WRAPPED_CALL(write, 0, b); }
bool HalFile::rename(const char* newPath) { HAL_FILE_WRAPPED_CALL(rename, false, newPath); }
bool HalFile::isDirectory() const { HAL_FILE_FORWARD_CALL(isDirectory, false, ); }
void HalFile::rewindDirectory() { HAL_FILE_WRAPPED_CALL(rewindDirectory, , ); }
bool HalFile::close() { HAL_FILE_WRAPPED_CALL(close, true, ); }
HalFile::DirectoryRead HalFile::openNextEntry(HalFile& entry) {
  HalStorage::StorageLock lock;
  if (!impl || &entry == this) return DirectoryRead::Error;
  if (!entry.impl) entry.impl = allocateImpl();
  if (!entry.impl) return DirectoryRead::Error;
  entry.impl->file.close();
  if (entry.impl->file.openNext(&impl->file)) return DirectoryRead::Record;
  return impl->file.getError() || entry.impl->file.getError() ? DirectoryRead::Error : DirectoryRead::End;
}
HalFile HalFile::openNextFile() {
  HalStorage::StorageLock lock;
  if (!impl) return {};
  auto next = allocateImpl();
  if (!next) return {};
  next->file = impl->file.openNextFile();
  return HalFile(std::move(next));
}
HalFile::DirectoryRead HalFile::readDirectoryRecord(uint8_t (&record)[32]) {
  HalStorage::StorageLock lock;
  memset(record, 0, sizeof(record));
  if (!impl || !impl->file.isDirectory() || impl->file.getError() || (impl->file.position() & 31u)) {
    LOG_ERR("STORAGE", "Invalid directory cursor");
    return DirectoryRead::Error;
  }
  // Unlike openNextFile, a single read does not internally skip an unbounded
  // run of deleted/LFN entries or conflate name-decoding failure with EOF.
  const int count = impl->file.read(record, sizeof(record));
  if (impl->file.getError() || count < 0 || (count != 0 && count != sizeof(record))) {
    memset(record, 0, sizeof(record));
    LOG_ERR("STORAGE", "Directory record read failed");
    return DirectoryRead::Error;
  }
  if (count == 0 || record[0] == 0) {
    memset(record, 0, sizeof(record));
    return DirectoryRead::End;
  }
  return DirectoryRead::Record;
}
bool HalFile::isOpen() const { return impl != nullptr && impl->file.isOpen(); }  // already thread-safe, no need to wrap
HalFile::operator bool() const { return isOpen(); }
