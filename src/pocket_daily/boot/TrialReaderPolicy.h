#pragma once
#include <cstddef>
#include <cstdint>

namespace PocketDaily::TrialReader {
enum class Stage : uint32_t { Prepared = 1, Reading, Leaving, Reopening, Missing, Recovering, Sleeping, Failed };
struct Record {
  uint32_t magic = 0x50445232, run = 0;
  Stage stage = Stage::Prepared;
  uint32_t requested = 0, rendered = 0, bytes = 0;
  int32_t spine = -1, page = -1;
  uint32_t visibleOffset = 0, hasVisibleOffset = 0;
  uint32_t reopened = 0, missingObserved = 0, recovered = 0;
  uint8_t sourceSHA256[32]{}, frameSHA256[32]{};
  uint32_t minFreeHeap = UINT32_MAX, minLargestBlock = UINT32_MAX, memorySamples = 0, renderMemorySamples = 0;
  uint32_t checksum = 0;
};
inline uint32_t checksum(const Record& r) {
  const auto* p = reinterpret_cast<const uint8_t*>(&r);
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < offsetof(Record, checksum); ++i) hash = (hash ^ p[i]) * 16777619u;
  return hash;
}
inline bool valid(const Record& r) {
  return r.magic == 0x50445232 && r.run && r.requested >= 2 && r.requested <= 20 && r.rendered <= r.requested &&
         r.bytes > 0 && r.bytes <= 4 * 1024 * 1024 && r.reopened <= 1 && r.missingObserved <= 1 && r.recovered <= 1 &&
         static_cast<uint32_t>(r.stage) >= 1 && static_cast<uint32_t>(r.stage) <= 8 && r.checksum == checksum(r);
}
inline bool progressed(int32_t spine, int32_t page, int32_t beforeSpine, int32_t beforePage) {
  return spine >= 0 && page > 0 && (spine > beforeSpine || (spine == beforeSpine && page > beforePage));
}
inline bool matches(const Record& r, int32_t spine, int32_t page, bool hasOffset, uint32_t offset) {
  return spine == r.spine && page == r.page && hasOffset == (r.hasVisibleOffset != 0) &&
         (!hasOffset || offset == r.visibleOffset);
}
inline bool readyForSleep(const Record& r) {
  return r.rendered == r.requested && r.reopened == 1 && r.missingObserved == 1 && r.recovered == 1 &&
         r.memorySamples == r.requested && r.renderMemorySamples == r.requested;
}
}  // namespace PocketDaily::TrialReader
