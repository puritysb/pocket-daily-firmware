#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace PocketDaily::TrialLease {
inline constexpr uint32_t kMagic = 0x50445434;
inline constexpr uint32_t kSlotSize = 0x640000;
inline constexpr uint32_t kSlot0 = 0x10000;
inline constexpr uint32_t kSlot1 = 0x650000;
inline constexpr uint32_t kX3Slot1 = 0x780000;
inline constexpr uint32_t kX3SlotSize = 0x770000;
inline constexpr uint32_t layoutBytes(uint32_t a, uint32_t b) {
  if ((a == kSlot0 && b == kSlot1) || (b == kSlot0 && a == kSlot1)) return kSlotSize;
  if ((a == kSlot0 && b == kX3Slot1) || (b == kSlot0 && a == kX3Slot1)) return kX3SlotSize;
  return 0;
}
inline constexpr char kCapability[] = "POCKET_TRIAL_LEASE_V4";
enum class Surface : uint32_t { Sync, FileTransfer, Home, Library, Settings, None };
inline constexpr uint32_t durationLimit(Surface surface) {
  switch (surface) {
    case Surface::Sync:
      return 900;
    case Surface::FileTransfer:
      return 900;
    case Surface::Home:
    case Surface::Library:
    case Surface::Settings:
      return 180;
    case Surface::None:
      return 0;
  }
  return 0;
}
inline const char* surfaceName(Surface surface) {
  switch (surface) {
    case Surface::Sync:
      return "sync";
    case Surface::FileTransfer:
      return "file-transfer";
    case Surface::Home:
      return "home";
    case Surface::Library:
      return "library";
    case Surface::Settings:
      return "settings";
    case Surface::None:
      return "none";
  }
  return "none";
}
inline Surface surfaceFromName(const char* name) {
  if (!name) return Surface::None;
  for (auto surface : {Surface::Sync, Surface::FileTransfer, Surface::Home, Surface::Library, Surface::Settings})
    if (std::strcmp(name, surfaceName(surface)) == 0) return surface;
  return Surface::None;
}
inline bool capturesUi(Surface surface) {
  switch (surface) {
    case Surface::Home:
    case Surface::Library:
    case Surface::Settings:
      return true;
    case Surface::Sync:
    case Surface::FileTransfer:
    case Surface::None:
      return false;
  }
  return false;
}
enum class Phase : uint32_t { Prepared = 1, Running, Returning, Returned, Failed };
enum class Failure : uint32_t {
  None,
  Unstarted,
  TrialImage,
  ReturnImage,
  RecordCommit,
  Timer,
  ReturnBudget,
  Selection,
  UnexpectedReset
};
struct Record {
  uint32_t magic = kMagic;
  uint32_t schema = 4;
  Phase phase = Phase::Prepared;
  uint32_t run = 0;
  uint32_t returnAddress = 0;
  uint32_t trialAddress = 0;
  uint32_t imageBytes = 0;
  uint32_t seconds = 0;
  uint32_t returnAttempts = 0;
  uint32_t resetReason = 0;
  uint32_t wakeCause = 0;
  uint8_t returnSHA256[32] = {};
  uint8_t trialSHA256[32] = {};
  Failure failure = Failure::None;
  Surface surface = Surface::Sync;
  uint32_t surfaceFree = 0, surfaceLargest = 0;
  uint32_t preHttpFree = 0, preHttpLargest = 0, readyFree = 0, readyLargest = 0;
  uint32_t checksum = 0;
};
static_assert(sizeof(Record) == 144);
inline uint32_t checksum(const Record& value) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
  uint32_t crc = UINT32_MAX;
  for (size_t i = 0; i < offsetof(Record, checksum); ++i) {
    crc ^= bytes[i];
    for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}
inline bool nonzero(const uint8_t* bytes) {
  uint8_t bits = 0;
  for (size_t i = 0; i < 32; ++i) bits |= bytes[i];
  return bits != 0;
}
inline bool valid(const Record& value) {
  const uint32_t bytes = layoutBytes(value.returnAddress, value.trialAddress);
  const auto phase = static_cast<uint32_t>(value.phase);
  return value.magic == kMagic && value.schema == 4 && value.run != 0 && bytes != 0 && value.imageBytes >= 65536 &&
         value.imageBytes <= bytes && value.seconds >= 30 && value.seconds <= durationLimit(value.surface) &&
         phase >= static_cast<uint32_t>(Phase::Prepared) && phase <= static_cast<uint32_t>(Phase::Failed) &&
         value.returnAttempts <= 2 &&
         static_cast<uint32_t>(value.failure) <= static_cast<uint32_t>(Failure::UnexpectedReset) &&
         nonzero(value.returnSHA256) && nonzero(value.trialSHA256) && value.checksum == checksum(value);
}
inline bool pending(Phase phase) {
  switch (phase) {
    case Phase::Prepared:
    case Phase::Running:
    case Phase::Returning:
      return true;
    case Phase::Returned:
    case Phase::Failed:
      return false;
  }
  return false;
}
inline bool allowsMaintenanceMode(const Record& record, uint32_t runningAddress, bool toSync) {
  // A preserved developer image can settle an ambiguous Prepared/Returning
  // record through one ordinary reboot. It cannot abandon a pending trial
  // into File Transfer or move a different running slot.
  return !pending(record.phase) || (toSync && runningAddress == record.returnAddress);
}
inline constexpr uint32_t kDeadlineMagic = 0x50445444;
inline void armDeadlineTicket(uint32_t& magic, uint32_t& target, uint32_t& payload, uint32_t run) {
  magic = 0;
  target = run;
  payload = run ^ kDeadlineMagic;
  __sync_synchronize();
  magic = kDeadlineMagic;
}
inline bool consumeDeadlineTicket(uint32_t& magic, uint32_t& target, uint32_t& payload, uint32_t run,
                                  bool softwareRestart) {
  const bool owned = magic == kDeadlineMagic;
  const bool validTicket = owned && softwareRestart && target == run && payload == (run ^ kDeadlineMagic);
  if (owned) {
    magic = 0;
    target = 0;
    payload = 0;
  }
  return validTicket;
}
inline bool decodeSHA(const char* text, uint8_t* bytes) {
  if (!text || std::strlen(text) != 64) return false;
  for (size_t i = 0; i < 32; ++i) {
    uint8_t value = 0;
    for (size_t j = 0; j < 2; ++j) {
      const char c = text[i * 2 + j];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
      value = static_cast<uint8_t>((value << 4) | (c <= '9' ? c - '0' : c - 'a' + 10));
    }
    bytes[i] = value;
  }
  return nonzero(bytes);
}
}  // namespace PocketDaily::TrialLease
