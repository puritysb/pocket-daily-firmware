#pragma once

#include <HalStorage.h>

// Flat-directory logical accounting, not allocated clusters/free space or a
// filesystem repair tool. Caller owns a dedicated cursor and excludes writes
// and remounts for the scan. No names, SDK types, or buffers escape the HAL.
class DirectoryAccounting {
 public:
  enum class State { Reading, Complete, Invalid, Unsupported, IoError, LimitExceeded };
  struct Limits {
    uint32_t slots;
    uint32_t files;
    uint32_t directories;
    uint64_t bytes;
  };
  struct Totals {
    uint32_t files = 0;
    uint32_t directories = 0;
    uint64_t bytes = 0;
  };
  DirectoryAccounting(HalStorage::FilesystemFormat format, Limits limits);
  // At most one 32-byte SDK read. Caller yields between steps as required.
  State step(HalFile& directory);
  // Pure parser seams; slots include deleted entries and end markers.
  State consume(const uint8_t (&record)[32]);
  State finish();
  State state() const { return current; }
  uint32_t slotsRead() const { return slots; }
  // Never publishes partial counts as a valid inventory.
  bool totals(Totals& output) const;

 private:
  State fat(const uint8_t* record);
  State exfat(const uint8_t* record);
  State add(bool directory, uint64_t bytes);
  HalStorage::FilesystemFormat format;
  Limits limits;
  Totals accumulated;
  State current = State::Reading;
  uint32_t slots = 0;
  uint64_t pendingBytes = 0;
  uint16_t checksum = 0, expectedChecksum = 0;
  uint8_t remaining = 0, nameEntries = 0, lfnChecksum = 0;
  bool lfn = false, stream = false, pendingDirectory = false;
};
