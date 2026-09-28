#include "ReadSyncStats.h"

#include <cstddef>
#include <cstring>

namespace Pocket::NearbySync::Stats {
namespace {
constexpr uint32_t MAGIC = 0x52535331u;  // "RSS1"

uint32_t checksum(const Record& record) {
  // FNV-1a over everything before `check`.
  const auto* bytes = reinterpret_cast<const uint8_t*>(&record);
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < offsetof(Record, check); ++i) {
    hash ^= bytes[i];
    hash *= 16777619u;
  }
  return hash;
}

uint16_t bump(const uint16_t value) { return value == UINT16_MAX ? value : static_cast<uint16_t>(value + 1); }
uint16_t add(const uint16_t value, const uint16_t more) {
  const uint32_t sum = static_cast<uint32_t>(value) + more;
  return sum > UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(sum);
}
}  // namespace

bool valid(const Record& record) { return record.magic == MAGIC && record.check == checksum(record); }

void seal(Record& record) {
  record.magic = MAGIC;
  record.check = checksum(record);
}

void begin(Record& record) {
  if (valid(record)) return;
  memset(&record, 0, sizeof(record));
  seal(record);
}

void startAttempt(Record& record, const uint8_t trigger, const uint8_t gate, const uint32_t freeHeap,
                  const uint32_t largestBlock, const bool opened) {
  begin(record);
  record.lastTrigger = trigger;
  record.lastGate = gate;
  record.startFree = freeHeap;
  record.startBlock = largestBlock;
  if (!opened) record.skipped = bump(record.skipped);
  seal(record);
}

void ready(Record& record, const uint32_t freeHeap, const uint32_t largestBlock, const bool accepted) {
  begin(record);
  record.openFree = freeHeap;
  record.openBlock = largestBlock;
  record.minFree = freeHeap;
  record.minBlock = largestBlock;
  if (accepted) {
    record.opened = bump(record.opened);
  } else {
    record.refused = bump(record.refused);
  }
  seal(record);
}

void sample(Record& record, const uint32_t freeHeap, const uint32_t largestBlock) {
  if (!valid(record)) return;
  if (freeHeap >= record.minFree && largestBlock >= record.minBlock) return;  // no write: most passes
  if (freeHeap < record.minFree) record.minFree = freeHeap;
  if (largestBlock < record.minBlock) record.minBlock = largestBlock;
  seal(record);
}

void closed(Record& record, const uint8_t reason, const uint16_t connections, const uint16_t lists,
            const uint16_t offers, const uint32_t freeHeap, const uint32_t largestBlock) {
  begin(record);
  record.lastClose = reason;
  record.connections = add(record.connections, connections);
  record.lists = add(record.lists, lists);
  record.offers = add(record.offers, offers);
  record.closedFree = freeHeap;
  record.closedBlock = largestBlock;
  seal(record);
}

}  // namespace Pocket::NearbySync::Stats
