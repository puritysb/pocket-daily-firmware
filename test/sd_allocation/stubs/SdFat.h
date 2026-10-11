#pragma once
#include <cstddef>
#include <cstdint>

using oflag_t = int;
constexpr oflag_t O_RDONLY = 0;
struct FsFile {};
struct FsBlockDeviceInterface {
  bool readSector(uint32_t, uint8_t*) { return false; }
};
namespace AllocationFake {
inline bool mounted = true;
inline bool present = true;
inline bool succeeds = true;
inline uint32_t sector = 0;
inline unsigned calls = 0;
}  // namespace AllocationFake
struct SpiCard {
  bool readSector(uint32_t sector, uint8_t* bytes) {
    AllocationFake::sector = sector;
    ++AllocationFake::calls;
    bytes[0] = 73;
    return AllocationFake::succeeds;
  }
};
struct FsVolume {
  uint8_t fatType() { return 32; }
  uint32_t clusterCount() { return 4096; }
  uint32_t bytesPerCluster() { return 4096; }
  uint32_t fatStartSector() { return 128; }
  FsFile open(const char*, oflag_t) { return {}; }
  bool mkdir(const char*, bool) { return true; }
  bool exists(const char*) { return true; }
  bool remove(const char*) { return true; }
  bool rmdir(const char*) { return true; }
  bool rename(const char*, const char*) { return true; }
};
struct SdFat : FsVolume {
  SpiCard cardValue;
  SpiCard* card() { return AllocationFake::present ? &cardValue : nullptr; }
};
