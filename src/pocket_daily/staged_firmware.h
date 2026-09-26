#pragma once

#include <cstddef>
#include <cstdint>

// Staged-firmware prompt rules (docs/nearby-sync-v1.md, "Firmware staging").
//
// The companion publishes a firmware image as `/update.bin` through the
// verified Pocket commit. Transport never flashes it. When the transfer
// session that published it ends (always a chip restart), the reader offers
// the existing SD firmware confirmation for that file exactly once.
//
// This header is free of Arduino types so the rules run in the host tests.
// The device keeps one Marker in RTC_NOINIT memory (StagedFirmwareStore):
// it survives the session's teardown restart but not power loss, so an old
// `/update.bin` on the card never produces a prompt by itself.
namespace PocketDaily::StagedFirmware {

// The single path a published firmware image uses.
inline constexpr char kPublishPath[] = "/update.bin";

// True only for the exact published firmware path.
bool isPublishTarget(const char* path);

// "A firmware image was published this session" record. RTC_NOINIT memory
// holds garbage after power-on, so every read is checked against a magic word
// and a check word derived from the payload.
struct Marker {
  uint32_t magic;
  uint32_t size;
  uint32_t crc32;
  uint32_t check;
};

void arm(Marker& marker, uint32_t size, uint32_t crc32);
// Reads and always clears the marker (one-shot). Returns true and fills
// size/crc32 only for a marker written by arm().
bool take(Marker& marker, uint32_t& size, uint32_t& crc32);

// Whether the boot that consumed a marker should offer the confirmation:
// only after a silent (session teardown) restart that lands on the Pocket
// Daily or Library shell, and only while the published file is still on the
// card with the recorded size.
struct OfferInput {
  bool markerValid = false;
  uint32_t markerSize = 0;
  bool silentRestart = false;
  bool landsOnShell = false;
  bool fileExists = false;
  uint64_t fileSize = 0;
};
bool shouldOffer(const OfferInput& input);

// Streams bytes of an image and extracts the value of its
// "CrossPoint version: <version>\0" marker (the same marker the companion
// requires before publishing). The first well-formed occurrence wins; a
// malformed candidate (non-printable, unterminated or over-long) is skipped.
class VersionScanner {
 public:
  static constexpr size_t kMaxVersion = 47;

  void reset();
  void feed(const uint8_t* bytes, size_t count);
  bool found() const { return state == State::Found; }
  // Empty until found().
  const char* version() const { return found() ? value : ""; }

 private:
  enum class State : uint8_t { Matching, Reading, Found };
  State state = State::Matching;
  size_t matched = 0;
  size_t length = 0;
  char value[kMaxVersion + 1] = {};
};

// Same build: identical, non-empty version strings. Dev builds embed the
// branch and short SHA, so equality means the staged image is the running one.
bool sameVersion(const char* staged, const char* running);

}  // namespace PocketDaily::StagedFirmware
