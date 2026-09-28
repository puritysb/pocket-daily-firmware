#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "ReadingSyncProtocol.h"

class NimBLECharacteristic;

namespace Pocket::NearbySync {

constexpr const char* SERVICE_UUID = "7b8d5001-8e5b-4a7e-9d9a-7e42d2c50001";
constexpr const char* STATUS_UUID = "7b8d5002-8e5b-4a7e-9d9a-7e42d2c50001";
constexpr const char* COMMAND_UUID = "7b8d5003-8e5b-4a7e-9d9a-7e42d2c50001";
constexpr const char* EVENT_UUID = "7b8d5004-8e5b-4a7e-9d9a-7e42d2c50001";
constexpr size_t MAX_RECORD_BYTES = RECORD_LIMIT;

// SCREEN: the Nearby Sync screen (passkey pairing, START_AP). WINDOW: a
// Reading Sync exchange window (docs/reading-sync-ble-v1.md): only already
// bonded peers, no pairing, no passkey, START_AP refused.
enum class Mode : uint8_t { SCREEN, WINDOW };

// A deliberately small BLE control plane. Callbacks only copy bounded records
// into a short queue; all state changes, SD and radio transitions happen from
// the owning loop.
class Service final {
 public:
  // `advertiseMs` 0 advertises until end().
  bool begin(const char* model, const char* firmware, Mode requestedMode = Mode::SCREEN, uint32_t advertiseMs = 120000);
  void end();

  bool isRunning() const { return running_; }
  Mode mode() const { return mode_; }
  bool isConnected() const { return connected_.load(std::memory_order_acquire); }
  // Encrypted and MITM-authenticated; in WINDOW mode also bonded.
  bool isAuthenticated() const { return authenticated_.load(std::memory_order_acquire); }
  uint32_t passkey() const { return passkey_; }
  const char* advertisedName() const { return advertisedName_; }
  const char* deviceId() const { return deviceId_; }
  // Changes on every connect and disconnect; queued records and exchange
  // state never outlive the connection that produced them.
  uint32_t connectionGeneration() const { return generation_.load(std::memory_order_acquire); }
  uint32_t connectedAtMs() const { return connectedAt_.load(std::memory_order_acquire); }

  // Takes the next well-formed command; false when none is queued. Malformed,
  // unknown and badly argued records are answered here (ERR ... BAD_COMMAND /
  // UNKNOWN_COMMAND / BAD_RECORD / BAD_CHUNK) and skipped. `command.chunk`
  // stays valid until the next call.
  bool takeCommand(ParsedCommand& command);
  // True once after the queue dropped a record (write-with-response chunks
  // arrived faster than the owner drained them).
  bool takeOverflow() { return queue_.takeOverflow(); }

  bool notifyOk(const char* requestId);
  bool notifyError(const char* requestId, const char* code);
  bool notifyHotspot(const char* requestId, const char* ssid, const char* passphrase, const char* host,
                     uint16_t httpPort, uint16_t wsPort, uint16_t leaseSeconds);
  // False when the stack could not queue it (retry on a later pass).
  bool notifyRecord(const char* record);
  // Drops the current link from the owning loop (unbonded grace expiry).
  void disconnect();

  // NimBLE callback entry points. These are public only to keep callback
  // adapters allocation-free and live in the implementation file.
  void onCommandRecord(const char* bytes, size_t length);
  void onConnected(bool connected, bool authenticated, uint16_t connHandle, uint32_t nowMs);
  void onAuthenticated(bool authenticated);

 private:
  bool running_ = false;
  Mode mode_ = Mode::SCREEN;
  uint32_t passkey_ = 0;
  char advertisedName_[20] = {};
  char deviceId_[9] = {};
  RecordQueue queue_;                        // NimBLE host task → owning loop
  char current_[MAX_RECORD_BYTES + 1] = {};  // the record takeCommand parsed
  std::atomic<bool> connected_{false};
  std::atomic<bool> authenticated_{false};
  std::atomic<uint32_t> generation_{0};
  std::atomic<uint32_t> connectedAt_{0};
  std::atomic<uint16_t> connHandle_{0xFFFF};
  NimBLECharacteristic* eventCharacteristic_ = nullptr;
};

}  // namespace Pocket::NearbySync
