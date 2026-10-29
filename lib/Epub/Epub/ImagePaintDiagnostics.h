#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "ImageDiagnostics.h"

// Developer-only paint observations, separate from layout/header diagnostics.
// Calls count BW/grayscale passes; success is not proof of the displayed pixels.
namespace epub_paint_diagnostics {
enum class Stage : uint32_t {
  None,
  LazyExtract,
  FileOpen,
  EmptyFile,
  DecoderFactory,
  PngHeapGate,
  PngAllocation,
  PngOpen,
  PngDimensions,
  PngCrop,
  PngInternalRow,
  PngBitDepth,
  PngGrayRow,
  PngLineAllocation,
  PngDecode,
  DecoderResult,
  Bounds
};
struct Snapshot {
  uint32_t run = 0, complete = 0, renderCalls = 0, cacheHits = 0;
  uint32_t lazyExtractAttempts = 0, lazyExtractFailures = 0, lastLazyExtractError = 0;
  uint32_t decoderAttempts = 0, decoderSuccesses = 0, placeholderCalls = 0, suppressedCalls = 0;
  uint32_t pngAttempts = 0, pngEntryFreeHeap = 0, pngEntryLargestBlock = 0, pngObjectBytes = 0, pngHeapGateBytes = 0;
  uint32_t firstFailureStage = 0;
  int32_t firstFailureCode = 0;
  uint32_t firstFailureFreeHeap = 0, firstFailureLargestBlock = 0;
};
class Observer {
 public:
  Snapshot value;
  void begin(uint32_t run) {
    value = {};
    value.run = run;
    decoding = false;
  }
  void rendered() {
    if (value.run) increment(value.renderCalls);
  }
  void cacheHit() {
    if (value.run) increment(value.cacheHits);
  }
  void lazyResult(bool success, uint32_t error, uint32_t freeHeap, uint32_t largestBlock) {
    if (!value.run) return;
    increment(value.lazyExtractAttempts);
    value.lastLazyExtractError = error;
    if (!success) {
      increment(value.lazyExtractFailures);
      fail(Stage::LazyExtract, static_cast<int32_t>(error), freeHeap, largestBlock);
    }
  }
  void decoderStarted() {
    if (value.run) {
      increment(value.decoderAttempts);
      decoding = true;
    }
  }
  void decoderResult(bool success, uint32_t freeHeap, uint32_t largestBlock) {
    if (!value.run || !decoding) return;
    if (success)
      increment(value.decoderSuccesses);
    else
      fail(Stage::DecoderResult, 0, freeHeap, largestBlock);
    decoding = false;
  }
  void placeholder(bool suppressed = false) {
    if (!value.run) return;
    increment(value.placeholderCalls);
    if (suppressed) increment(value.suppressedCalls);
  }
  void pngStarted(uint32_t freeHeap, uint32_t largestBlock, uint32_t objectBytes, uint32_t gateBytes) {
    if (!value.run || !decoding) return;
    if (!value.pngAttempts) {
      value.pngEntryFreeHeap = freeHeap;
      value.pngEntryLargestBlock = largestBlock;
      value.pngObjectBytes = objectBytes;
      value.pngHeapGateBytes = gateBytes;
    }
    increment(value.pngAttempts);
  }
  void pngFailed(Stage stage, int32_t code, uint32_t freeHeap, uint32_t largestBlock) {
    if (decoding) fail(stage, code, freeHeap, largestBlock);
  }
  void fail(Stage stage, int32_t code, uint32_t freeHeap, uint32_t largestBlock) {
    if (!value.run || value.firstFailureStage || stage == Stage::None) return;
    value.firstFailureStage = static_cast<uint32_t>(stage);
    value.firstFailureCode = code;
    value.firstFailureFreeHeap = freeHeap;
    value.firstFailureLargestBlock = largestBlock;
  }

 private:
  bool decoding = false;
  static void increment(uint32_t& value) {
    if (value != UINT32_MAX) ++value;
  }
};
static_assert(sizeof(Observer) <= 96);
inline constexpr size_t RECORD_BYTES = 88;
inline void encode(const Snapshot& s, uint8_t (&out)[RECORD_BYTES]) {
  const uint32_t fields[] = {0x31504450,
                             s.run,
                             s.complete,
                             s.renderCalls,
                             s.cacheHits,
                             s.lazyExtractAttempts,
                             s.lazyExtractFailures,
                             s.lastLazyExtractError,
                             s.decoderAttempts,
                             s.decoderSuccesses,
                             s.placeholderCalls,
                             s.suppressedCalls,
                             s.pngAttempts,
                             s.pngEntryFreeHeap,
                             s.pngEntryLargestBlock,
                             s.pngObjectBytes,
                             s.pngHeapGateBytes,
                             s.firstFailureStage,
                             static_cast<uint32_t>(s.firstFailureCode),
                             s.firstFailureFreeHeap,
                             s.firstFailureLargestBlock};
  for (size_t i = 0; i < 21; ++i)
    for (size_t j = 0; j < 4; ++j) out[i * 4 + j] = static_cast<uint8_t>(fields[i] >> (j * 8));
  const auto sum = epub_image_diagnostics::checksum(out, RECORD_BYTES - 4);
  for (size_t j = 0; j < 4; ++j) out[84 + j] = static_cast<uint8_t>(sum >> (j * 8));
}
inline bool decode(const uint8_t* bytes, size_t size, uint32_t expectedRun, Snapshot& out) {
  if (!bytes || size != RECORD_BYTES || !expectedRun) return false;
  uint32_t f[22]{};
  for (size_t i = 0; i < 22; ++i)
    for (size_t j = 0; j < 4; ++j) f[i] |= static_cast<uint32_t>(bytes[i * 4 + j]) << (j * 8);
  if (f[0] != 0x31504450 || f[1] != expectedRun || f[2] > 1 || f[7] > 9 ||
      f[17] > static_cast<uint32_t>(Stage::Bounds) ||
      f[21] != epub_image_diagnostics::checksum(bytes, RECORD_BYTES - 4) || f[4] > f[3] || f[5] > f[3] || f[6] > f[5] ||
      f[8] > f[3] || f[9] > f[8] || f[10] > f[3] || f[11] > f[10] || f[12] > f[8] ||
      (!f[17] && (f[18] || f[19] || f[20])) || (!f[12] && (f[13] || f[14] || f[15] || f[16])))
    return false;
  int32_t code;
  memcpy(&code, &f[18], sizeof(code));
  out = {f[1],  f[2],  f[3],  f[4],  f[5],  f[6],  f[7],  f[8], f[9],  f[10],
         f[11], f[12], f[13], f[14], f[15], f[16], f[17], code, f[19], f[20]};
  return true;
}
inline constexpr size_t MAX_JSON_BYTES = 640;
inline int formatJson(char* out, size_t capacity, const Snapshot* s) {
  if (!s) return snprintf(out, capacity, "null");
  return snprintf(
      out, capacity,
      "{\"version\":1,\"run\":%lu,\"complete\":%s,\"renderCalls\":%lu,\"cacheHits\":%lu,"
      "\"lazyExtractAttempts\":%lu,\"lazyExtractFailures\":%lu,\"lastLazyExtractError\":%lu,"
      "\"decoderAttempts\":%lu,\"decoderSuccesses\":%lu,\"placeholderCalls\":%lu,\"suppressedCalls\":%lu,"
      "\"pngAttempts\":%lu,\"pngEntryFreeHeap\":%lu,\"pngEntryLargestBlock\":%lu,\"pngObjectBytes\":%lu,"
      "\"pngHeapGateBytes\":%lu,\"firstFailureStage\":%lu,\"firstFailureCode\":%ld,"
      "\"firstFailureFreeHeap\":%lu,\"firstFailureLargestBlock\":%lu}",
      static_cast<unsigned long>(s->run), s->complete ? "true" : "false", static_cast<unsigned long>(s->renderCalls),
      static_cast<unsigned long>(s->cacheHits), static_cast<unsigned long>(s->lazyExtractAttempts),
      static_cast<unsigned long>(s->lazyExtractFailures), static_cast<unsigned long>(s->lastLazyExtractError),
      static_cast<unsigned long>(s->decoderAttempts), static_cast<unsigned long>(s->decoderSuccesses),
      static_cast<unsigned long>(s->placeholderCalls), static_cast<unsigned long>(s->suppressedCalls),
      static_cast<unsigned long>(s->pngAttempts), static_cast<unsigned long>(s->pngEntryFreeHeap),
      static_cast<unsigned long>(s->pngEntryLargestBlock), static_cast<unsigned long>(s->pngObjectBytes),
      static_cast<unsigned long>(s->pngHeapGateBytes), static_cast<unsigned long>(s->firstFailureStage),
      static_cast<long>(s->firstFailureCode), static_cast<unsigned long>(s->firstFailureFreeHeap),
      static_cast<unsigned long>(s->firstFailureLargestBlock));
}
#ifdef ENABLE_DEV_REMOTE_FLASH
inline Observer active;
#endif
}  // namespace epub_paint_diagnostics
