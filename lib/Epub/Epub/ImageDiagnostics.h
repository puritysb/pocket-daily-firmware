#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>

// Developer observation only: no filenames, pixels, allocations or writes in
// the image hooks. Counts include layout ahead of the currently painted page.
namespace epub_image_diagnostics {
struct Snapshot {
  uint32_t run = 0, mode = 0, seen = 0, probeStreamFailures = 0, dimensionFailures = 0;
  uint32_t lastProbeError = 0, extractAttempts = 0, extractFailures = 0, lastExtractError = 0;
  uint32_t altFallbacks = 0, imageBlocks = 0, beforeProbeFreeHeap = 0, beforeProbeLargestBlock = 0;
  uint32_t complete = 0;
};
class Observer {
 public:
  Snapshot value;
  void begin(uint32_t run, uint32_t mode) {
    value = {};
    value.run = run;
    value.mode = mode;
  }
  void imageSeen() {
    if (value.run) increment(value.seen);
  }
  void probeStarted(uint32_t freeHeap, uint32_t largestBlock) {
    if (!value.run) return;
    value.beforeProbeFreeHeap = freeHeap;
    value.beforeProbeLargestBlock = largestBlock;
  }
  void probeResult(bool streamed, bool dimensions, uint32_t error) {
    if (!value.run) return;
    if (!streamed) increment(value.probeStreamFailures);
    if (!dimensions) increment(value.dimensionFailures);
    value.lastProbeError = error;
  }
  void extractionStarted() {
    if (value.run) increment(value.extractAttempts);
  }
  void extractionResult(bool extracted, uint32_t error) {
    if (!value.run) return;
    if (!extracted) increment(value.extractFailures);
    value.lastExtractError = error;
  }
  void altFallback() {
    if (value.run) increment(value.altFallbacks);
  }
  void imageBlock() {
    if (value.run) increment(value.imageBlocks);
  }

 private:
  static void increment(uint32_t& value) {
    if (value != UINT32_MAX) ++value;
  }
};
static_assert(sizeof(Observer) <= 64, "image observation must remain scalar and bounded");
inline constexpr size_t RECORD_BYTES = 64;
inline uint32_t checksum(const uint8_t* data, size_t count) {
  uint32_t result = 2166136261u;
  for (size_t i = 0; i < count; ++i) result = (result ^ data[i]) * 16777619u;
  return result;
}
inline void encode(const Snapshot& s, uint8_t (&out)[RECORD_BYTES]) {
  const uint32_t fields[] = {0x31494450,
                             s.run,
                             s.mode,
                             s.seen,
                             s.probeStreamFailures,
                             s.dimensionFailures,
                             s.lastProbeError,
                             s.extractAttempts,
                             s.extractFailures,
                             s.lastExtractError,
                             s.altFallbacks,
                             s.imageBlocks,
                             s.beforeProbeFreeHeap,
                             s.beforeProbeLargestBlock,
                             s.complete};
  for (size_t i = 0; i < 15; ++i)
    for (size_t j = 0; j < 4; ++j) out[i * 4 + j] = static_cast<uint8_t>(fields[i] >> (j * 8));
  const auto sum = checksum(out, RECORD_BYTES - 4);
  for (size_t j = 0; j < 4; ++j) out[60 + j] = static_cast<uint8_t>(sum >> (j * 8));
}
inline bool decode(const uint8_t* bytes, size_t size, uint32_t expectedRun, Snapshot& out) {
  if (!bytes || size != RECORD_BYTES || !expectedRun) return false;
  uint32_t fields[16]{};
  for (size_t i = 0; i < 16; ++i)
    for (size_t j = 0; j < 4; ++j) fields[i] |= static_cast<uint32_t>(bytes[i * 4 + j]) << (j * 8);
  if (fields[0] != 0x31494450 || fields[1] != expectedRun || fields[2] > 2 || fields[6] > 9 || fields[9] > 9 ||
      fields[14] > 1 || fields[15] != checksum(bytes, RECORD_BYTES - 4) || fields[4] > fields[3] ||
      fields[5] > fields[3] || fields[7] > fields[5] || fields[8] > fields[7] || fields[10] > fields[3] ||
      fields[11] > fields[3])
    return false;
  out = {fields[1], fields[2], fields[3],  fields[4],  fields[5],  fields[6],  fields[7],
         fields[8], fields[9], fields[10], fields[11], fields[12], fields[13], fields[14]};
  return true;
}
inline constexpr size_t MAX_JSON_BYTES = 512;
inline int formatJson(char* out, size_t capacity, const Snapshot* s) {
  if (!s) return snprintf(out, capacity, "null");
  return snprintf(out, capacity,
                  "{\"version\":1,\"run\":%lu,\"mode\":%lu,\"complete\":%s,\"seen\":%lu,"
                  "\"probeStreamFailures\":%lu,\"dimensionFailures\":%lu,\"lastProbeError\":%lu,"
                  "\"extractAttempts\":%lu,\"extractFailures\":%lu,\"lastExtractError\":%lu,"
                  "\"altFallbacks\":%lu,\"imageBlocks\":%lu,\"beforeProbeFreeHeap\":%lu,"
                  "\"beforeProbeLargestBlock\":%lu}",
                  static_cast<unsigned long>(s->run), static_cast<unsigned long>(s->mode),
                  s->complete ? "true" : "false", static_cast<unsigned long>(s->seen),
                  static_cast<unsigned long>(s->probeStreamFailures), static_cast<unsigned long>(s->dimensionFailures),
                  static_cast<unsigned long>(s->lastProbeError), static_cast<unsigned long>(s->extractAttempts),
                  static_cast<unsigned long>(s->extractFailures), static_cast<unsigned long>(s->lastExtractError),
                  static_cast<unsigned long>(s->altFallbacks), static_cast<unsigned long>(s->imageBlocks),
                  static_cast<unsigned long>(s->beforeProbeFreeHeap),
                  static_cast<unsigned long>(s->beforeProbeLargestBlock));
}
#ifdef ENABLE_DEV_REMOTE_FLASH
inline Observer active;
#endif
}  // namespace epub_image_diagnostics
