#pragma once
#include "../../stubs/HalStorage.h"

// Add the device's convenience read API to the shared in-memory SD fake.
class HomeInputsStorage : public HalStorage {
 public:
  bool openFileForRead(const char*, const char* path, HalFile& file) {
    file = open(path, O_RDONLY);
    return static_cast<bool>(file);
  }
};
inline HomeInputsStorage homeInputsStorage;
#undef Storage
#define Storage homeInputsStorage
