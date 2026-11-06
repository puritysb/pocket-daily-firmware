#pragma once

// Host stub: removeBookFile() deletes through the HAL before clearing caches.
struct HalStorageStub {
  bool remove(const char*) { return true; }
};
inline HalStorageStub Storage;
