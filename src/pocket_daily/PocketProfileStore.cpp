#include "PocketProfileStore.h"

#include <HalStorage.h>

#include <cstring>

namespace PocketDaily::DailyProfile {
namespace {
Profile gCurrent = defaults();
uint32_t gGeneration = 0;

bool readSlot(const int slot, Profile& profile, uint32_t& generation) {
  HalFile file = Storage.open(SLOT_PATHS[slot], O_RDONLY);
  uint8_t bytes[RECORD_BYTES];
  return file && file.size() == sizeof(bytes) && file.read(bytes, sizeof(bytes)) == static_cast<int>(sizeof(bytes)) &&
         decodeRecord(bytes, sizeof(bytes), profile, generation);
}

// Index of the newest valid slot, or -1.
int newest(Profile (&profiles)[2], uint32_t (&generations)[2]) {
  const bool valid0 = readSlot(0, profiles[0], generations[0]);
  const bool valid1 = readSlot(1, profiles[1], generations[1]);
  if (valid0 && valid1) return generations[1] > generations[0] ? 1 : 0;
  return valid0 ? 0 : (valid1 ? 1 : -1);
}
}  // namespace

void loadAtBoot() {
  Profile profiles[2];
  uint32_t generations[2]{};
  const int slot = newest(profiles, generations);
  gCurrent = slot < 0 ? defaults() : profiles[slot];
  gGeneration = slot < 0 ? 0 : generations[slot];
}

const Profile& current() { return gCurrent; }
uint32_t generation() { return gGeneration; }

SaveResult save(const Profile& profile, const uint32_t expectedGeneration) {
  if (!valid(profile)) return SaveResult::Invalid;
  if (expectedGeneration != gGeneration) return SaveResult::Conflict;
  if (gGeneration == UINT32_MAX) return SaveResult::StorageError;  // no ambiguous wraparound
  Profile profiles[2];
  uint32_t generations[2]{};
  const int latest = newest(profiles, generations);
  // Never overwrite the slot holding the profile in use.
  const int target = latest == 0 ? 1 : 0;
  const uint32_t next = gGeneration + 1;
  uint8_t bytes[RECORD_BYTES];
  if (!encodeRecord(profile, next, bytes)) return SaveResult::Invalid;
  {
    HalFile file = Storage.open(SLOT_PATHS[target], O_WRITE | O_CREAT | O_TRUNC);
    if (!file || file.write(bytes, sizeof(bytes)) != sizeof(bytes)) return SaveResult::StorageError;
  }
  Profile verified;
  uint32_t verifiedGeneration = 0;
  if (!readSlot(target, verified, verifiedGeneration) || verified != profile || verifiedGeneration != next)
    return SaveResult::StorageError;
  gCurrent = profile;
  gGeneration = next;
  return SaveResult::Ok;
}

void resetForTest() {
  gCurrent = defaults();
  gGeneration = 0;
}
}  // namespace PocketDaily::DailyProfile
