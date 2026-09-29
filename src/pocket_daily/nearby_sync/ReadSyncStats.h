#pragma once

#include <cstdint>

// Heap and outcome figures of the reading-sync exchange windows
// (docs/reading-sync-ble-v1.md, "Memory"), kept in RTC_NOINIT memory by the
// window controller so they survive the restart into a Wi-Fi mode and can be
// read from /api/status. Garbage after power-on is rejected by magic + check.
namespace Pocket::NearbySync::Stats {

struct Record {
  uint32_t magic;
  uint16_t opened;       // windows whose radio came up
  uint16_t skipped;      // windows not started by a gate
  uint16_t refused;      // radio up but below the ready floor
  uint16_t connections;  // phone connections served while open
  uint16_t lists;        // READ_LIST answered
  uint16_t offers;       // offers stored
  uint8_t lastTrigger;   // Trigger
  uint8_t lastGate;      // Gate of the last start attempt
  uint8_t lastClose;     // CloseReason of the last open window
  uint8_t reserved;
  uint32_t startFree;  // before NimBLE, last start attempt
  uint32_t startBlock;
  uint32_t openFree;  // NimBLE up and advertising
  uint32_t openBlock;
  uint32_t minFree;  // lowest while open (connections, exchanges)
  uint32_t minBlock;
  uint32_t closedFree;  // after deinit: shows whether memory came back
  uint32_t closedBlock;
  uint32_t check;
};

// Valid record or zeroed fresh one.
void begin(Record& record);
bool valid(const Record& record);
void seal(Record& record);

void startAttempt(Record& record, uint8_t trigger, uint8_t gate, uint32_t freeHeap, uint32_t largestBlock, bool opened);
void ready(Record& record, uint32_t freeHeap, uint32_t largestBlock, bool accepted);
void sample(Record& record, uint32_t freeHeap, uint32_t largestBlock);
void closed(Record& record, uint8_t reason, uint16_t connections, uint16_t lists, uint16_t offers, uint32_t freeHeap,
            uint32_t largestBlock);

}  // namespace Pocket::NearbySync::Stats
