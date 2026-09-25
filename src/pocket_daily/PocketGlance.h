#pragma once

#include <cstddef>
#include <cstdint>

#include "models.h"

// App-provided weather and today's events (docs/pocket-glance-v1.md). The
// companion composes the document; the reader validates, stores and draws it.
// Pure model and codecs; storage lives in PocketGlanceStore.
namespace PocketDaily::AppGlance {
inline constexpr size_t MAX_BODY_BYTES = 2048;
// Earliest plausible wall clock (2023-11-14); also the "clock is set" floor.
inline constexpr uint32_t MIN_EPOCH = 1700000000;
inline constexpr int MIN_UTC_OFFSET_MINUTES = -720;
inline constexpr int MAX_UTC_OFFSET_MINUTES = 840;

// The stored glance plus when and where (UTC offset) the app composed it.
struct Snapshot {
  Glance glance;
  uint32_t savedEpoch;
  int16_t utcOffsetMinutes;
  char syncedHm[6];
  void clear();
};

// Field rules shared by the JSON parser and the record decoder: counts, ranges,
// "HH:MM" / ISO dates, strict UTF-8 without controls, non-empty event titles.
bool valid(const Snapshot& snapshot);

// Strict document parser (PocketGlanceJson.cpp): any unknown or missing key,
// wrong type, out-of-range value, oversize string or bad schema rejects the
// whole document; `out` is cleared on failure, so nothing is partially
// applied. `error` receives a short static reason; `outOfMemory` is set when
// parsing failed for lack of heap.
bool parseJson(const char* json, size_t length, Snapshot& out, const char*& error, bool& outOfMemory);

// Persisted record ("PDGL", version, payload, CRC32), little-endian fixed
// layout independent of struct padding. Usage/wrap-up fields are not stored.
inline constexpr size_t RECORD_BYTES = 404;
bool encodeRecord(const Snapshot& snapshot, uint8_t (&bytes)[RECORD_BYTES]);
// Validates the CRC and every field; `snapshot` is cleared on failure.
bool decodeRecord(const uint8_t* bytes, size_t size, Snapshot& snapshot);
}  // namespace PocketDaily::AppGlance
