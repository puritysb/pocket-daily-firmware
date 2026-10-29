#pragma once

#include <cstddef>
#include <cstdint>

#include "pocket_daily/ReaderPerf.h"

namespace PocketDaily::ReaderBenchmark {
inline constexpr size_t HEADER_BYTES = 32;
inline constexpr size_t RECORD_BYTES = 48;
inline constexpr uint32_t MAX_PAGES = ReaderPerf::RING_SIZE;
inline constexpr size_t MAX_BYTES = HEADER_BYTES + MAX_PAGES * RECORD_BYTES;
inline constexpr uint32_t MAGIC = 0x4d424450;  // PDBM

inline uint32_t checksum(const uint8_t* bytes, size_t size) {
  uint32_t value = 2166136261u;
  for (size_t i = 0; i < size; ++i) value = (value ^ bytes[i]) * 16777619u;
  return value;
}
inline void put(uint8_t* bytes, uint32_t value, size_t size = 4) {
  for (size_t i = 0; i < size; ++i) bytes[i] = static_cast<uint8_t>(value >> (8 * i));
}
inline uint32_t get(const uint8_t* bytes) {
  uint32_t value = 0;
  for (size_t i = 0; i < 4; ++i) value |= static_cast<uint32_t>(bytes[i]) << (8 * i);
  return value;
}
inline bool encodeHeader(uint32_t run, uint32_t pages, uint8_t (&out)[HEADER_BYTES]) {
  if (!run || !pages || pages > MAX_PAGES) return false;
  const uint32_t fields[] = {MAGIC, 1, run, pages, RECORD_BYTES, 0, 0};
  for (size_t i = 0; i < 7; ++i) put(out + i * 4, fields[i]);
  put(out + 28, checksum(out, 28));
  return true;
}
inline bool validHeader(const uint8_t* bytes, size_t size, uint32_t run, uint32_t pages) {
  return bytes && run && pages && pages <= MAX_PAGES && size == HEADER_BYTES + pages * RECORD_BYTES &&
         get(bytes) == MAGIC && get(bytes + 4) == 1 && get(bytes + 8) == run && get(bytes + 12) == pages &&
         get(bytes + 16) == RECORD_BYTES && !get(bytes + 20) && !get(bytes + 24) &&
         get(bytes + 28) == checksum(bytes, 28);
}
inline void encodeRecord(const ReaderPerf::TurnRecord& record, uint16_t index, uint8_t (&out)[RECORD_BYTES]) {
  size_t offset = 0;
  for (const uint16_t value : record.ms) {
    put(out + offset, value, 2);
    offset += 2;
  }
  for (const uint16_t value : record.counters) {
    put(out + offset, value, 2);
    offset += 2;
  }
  out[offset++] = record.strips;
  out[offset++] = record.flags;
  put(out + offset, record.freeHeap);
  put(out + offset + 4, record.largestBlock);
  put(out + offset + 8, index, 2);
  put(out + 44, checksum(out, 44));
}
static_assert(ReaderPerf::STAGE_COUNT == 12 && ReaderPerf::COUNTER_COUNT == 4,
              "change the benchmark format when telemetry fields change");
}  // namespace PocketDaily::ReaderBenchmark
