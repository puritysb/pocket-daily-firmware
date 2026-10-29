#pragma once
#include <algorithm>
#include <cstring>

#include "FakeSDK.h"
struct HalFile {
  size_t offset = 0;
  size_t size() const { return Fake::staged.size(); }
  int read(uint8_t* out, size_t n) {
    n = std::min(n, Fake::staged.size() - offset);
    std::memcpy(out, Fake::staged.data() + offset, n);
    offset += n;
    return static_cast<int>(n);
  }
};
struct Store {
  bool openFileForRead(const char*, const char*, HalFile& f) {
    f.offset = 0;
    return !Fake::staged.empty();
  }
};
inline Store Storage;
