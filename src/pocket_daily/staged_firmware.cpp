#include "staged_firmware.h"

#include <cstring>

namespace PocketDaily::StagedFirmware {

namespace {
constexpr uint32_t kMagic = 0x50465755U;  // "PFWU"
constexpr uint32_t kCheckSalt = 0xA5C3F00DU;
constexpr char kVersionPrefix[] = "CrossPoint version: ";
constexpr size_t kVersionPrefixLength = sizeof(kVersionPrefix) - 1;

uint32_t checkWord(const uint32_t size, const uint32_t crc32) { return (kMagic ^ size ^ ~crc32) + kCheckSalt; }
}  // namespace

bool isPublishTarget(const char* path) { return path && strcmp(path, kPublishPath) == 0; }

void arm(Marker& marker, const uint32_t size, const uint32_t crc32) {
  marker.size = size;
  marker.crc32 = crc32;
  marker.check = checkWord(size, crc32);
  marker.magic = kMagic;
}

bool take(Marker& marker, uint32_t& size, uint32_t& crc32) {
  const bool valid = marker.magic == kMagic && marker.size != 0 && marker.check == checkWord(marker.size, marker.crc32);
  if (valid) {
    size = marker.size;
    crc32 = marker.crc32;
  }
  marker = Marker{};
  return valid;
}

bool shouldOffer(const OfferInput& input) {
  return input.markerValid && input.silentRestart && input.landsOnShell && input.fileExists && input.markerSize != 0 &&
         input.fileSize == input.markerSize;
}

void VersionScanner::reset() {
  state = State::Matching;
  matched = 0;
  length = 0;
  value[0] = '\0';
}

void VersionScanner::feed(const uint8_t* bytes, const size_t count) {
  if (!bytes) return;
  for (size_t i = 0; i < count && state != State::Found; ++i) {
    const auto c = static_cast<char>(bytes[i]);
    if (state == State::Reading) {
      if (c == '\0' && length > 0) {
        value[length] = '\0';
        state = State::Found;
        continue;
      }
      if (c > ' ' && c < 0x7F && length < kMaxVersion) {
        value[length++] = c;
        continue;
      }
      // Malformed candidate: resume matching at this byte.
      state = State::Matching;
      matched = 0;
      length = 0;
    }
    // 'C' occurs only at the start of the prefix, so a mismatch can restart
    // the match at the current byte without a failure table.
    if (c == kVersionPrefix[matched]) {
      if (++matched == kVersionPrefixLength) {
        state = State::Reading;
        matched = 0;
        length = 0;
      }
    } else {
      matched = c == kVersionPrefix[0] ? 1 : 0;
    }
  }
}

bool sameVersion(const char* staged, const char* running) {
  return staged && running && staged[0] && strcmp(staged, running) == 0;
}

}  // namespace PocketDaily::StagedFirmware
