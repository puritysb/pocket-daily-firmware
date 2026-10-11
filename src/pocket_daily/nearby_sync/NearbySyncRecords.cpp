#include <cstdio>
#include <cstring>

#include "NearbySyncService.h"

// The NimBLE-free half of the Nearby Sync service: connection state, the
// callback record queue, command parsing and reply records. Host tests compile
// this file with a fake radio (test/reading_sync_ble); NearbySyncService.cpp
// holds the NimBLE half.
namespace Pocket::NearbySync {

void Service::onConnected(const bool connected, const bool authenticated, const uint16_t connHandle,
                          const uint32_t nowMs) {
  if (connected) connections_.fetch_add(1, std::memory_order_acq_rel);
  connHandle_.store(connected ? connHandle : 0xFFFF, std::memory_order_release);
  connectedAt_.store(nowMs, std::memory_order_release);
  authenticated_.store(connected && authenticated, std::memory_order_release);
  connected_.store(connected, std::memory_order_release);
  generation_.fetch_add(1, std::memory_order_acq_rel);
}

void Service::onAuthenticated(const bool authenticated) {
  authenticated_.store(connected_.load(std::memory_order_acquire) && authenticated, std::memory_order_release);
}

void Service::onCommandRecord(const char* bytes, const size_t length) {
  queue_.push(bytes, length, connectionGeneration());
}

bool Service::takeCommand(ParsedCommand& command) {
  memset(current_, 0, sizeof(current_));
  size_t length = 0;
  while (queue_.pop(current_, sizeof(current_), length, connectionGeneration())) {
    switch (parseCommand(current_, length, command)) {
      case ParseResult::OK:
        return true;
      case ParseResult::MALFORMED:
        notifyError("00000000", "BAD_COMMAND");
        break;
      case ParseResult::UNKNOWN_VERB:
        notifyError(command.requestId, "UNKNOWN_COMMAND");
        break;
      case ParseResult::BAD_FIELDS:
        notifyError(command.requestId, badFieldsCode(command.verb));
        break;
    }
  }
  return false;
}

bool Service::notifyOk(const char* requestId) {
  char record[32];
  if (!formatOk(requestId, record, sizeof(record))) return false;
  return notifyRecord(record);
}

bool Service::notifyError(const char* requestId, const char* code) {
  char record[80];
  if (!formatError(requestId, code ? code : "INTERNAL", record, sizeof(record))) return false;
  return notifyRecord(record);
}

bool Service::notifyHotspot(const char* requestId, const char* ssid, const char* passphrase, const char* host,
                            const uint16_t httpPort, const uint16_t wsPort, const uint16_t leaseSeconds) {
  char record[MAX_RECORD_BYTES + 1];
  const int length = snprintf(record, sizeof(record), "AP %s %s %s %s %u %u %u", requestId, ssid, passphrase, host,
                              httpPort, wsPort, leaseSeconds);
  if (length <= 0 || static_cast<size_t>(length) > MAX_RECORD_BYTES) return false;
  return notifyRecord(record);
}

}  // namespace Pocket::NearbySync
