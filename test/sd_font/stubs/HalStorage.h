#pragma once
#include "../../deck_store/stubs/HalStorage.h"

// Font-specific adapter over the shared fake SD; production uses locked HAL.
class FontFile : public HalFile {
 public:
  FontFile() = default;
  explicit FontFile(HalFile file) : HalFile(file) {}
  bool seekSet(size_t offset) { return seek(offset); }
  void close() { *this = FontFile(); }
};
class FontStorage {
 public:
  bool openFileForRead(const char*, const char* path, FontFile& file) {
    file = FontFile(HalStorage::getInstance().open(path, O_RDONLY));
    return static_cast<bool>(file);
  }
};
inline FontStorage fontStorage;
#undef Storage
#define Storage fontStorage
#define HalFile FontFile
