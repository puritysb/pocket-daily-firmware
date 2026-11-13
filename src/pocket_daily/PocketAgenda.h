#pragma once
#include <cstddef>
#include <cstdint>

#include "models.h"
namespace PocketDaily::Agenda {
inline constexpr uint8_t DAY_CAP = 7;
inline constexpr size_t MAX_BODY_BYTES = 8192;
inline constexpr size_t RECORD_BYTES = 1343;
struct Day {
  uint32_t date = 0;
  uint16_t omittedEventCount = 0;
  uint8_t eventCount = 0;
  Event events[Glance::EVENT_CAP]{};
};
struct Snapshot {
  uint8_t dayCount = 0;
  Day days[DAY_CAP]{};
  void clear();
};
bool valid(const Snapshot& snapshot);
uint32_t parseDate(const char* iso);
uint32_t shiftDate(uint32_t date, int direction);
const Day* findDay(const Snapshot& snapshot, uint32_t date);
bool encodeRecord(const Snapshot& snapshot, uint8_t (&bytes)[RECORD_BYTES]);
bool decodeRecord(const uint8_t* bytes, size_t size, Snapshot& snapshot);
bool parseJson(const char* json, size_t size, Snapshot& snapshot, const char*& error, bool& outOfMemory);
// Caller-owned output; false rather than emitting a truncated document.
bool writeJson(const Snapshot& snapshot, char* bytes, size_t capacity, size_t& size);
}  // namespace PocketDaily::Agenda
