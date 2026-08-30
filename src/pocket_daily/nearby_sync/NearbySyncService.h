#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

class NimBLECharacteristic;

namespace Pocket::NearbySync {

constexpr const char* SERVICE_UUID = "7b8d5001-8e5b-4a7e-9d9a-7e42d2c50001";
constexpr const char* STATUS_UUID = "7b8d5002-8e5b-4a7e-9d9a-7e42d2c50001";
constexpr const char* COMMAND_UUID = "7b8d5003-8e5b-4a7e-9d9a-7e42d2c50001";
constexpr const char* EVENT_UUID = "7b8d5004-8e5b-4a7e-9d9a-7e42d2c50001";
constexpr size_t MAX_RECORD_BYTES = 220;

enum class CommandType : uint8_t { NONE, PING, START_AP, CANCEL };

struct Command {
  CommandType type = CommandType::NONE;
  char requestId[9] = {};
};

// A deliberately small BLE control plane. Callbacks only copy bounded records;
// all state changes and radio transitions happen from the owning activity loop.
class Service final {
 public:
  bool begin(const char* model, const char* firmware);
  void end();

  bool isRunning() const { return running_; }
  bool isConnected() const { return connected_.load(std::memory_order_acquire); }
  bool isAuthenticated() const { return authenticated_.load(std::memory_order_acquire); }
  uint32_t passkey() const { return passkey_; }
  const char* advertisedName() const { return advertisedName_; }
  const char* deviceId() const { return deviceId_; }

  bool takeCommand(Command& command);
  bool notifyOk(const char* requestId);
  bool notifyError(const char* requestId, const char* code);
  bool notifyHotspot(const char* requestId, const char* ssid, const char* passphrase, const char* host,
                     uint16_t httpPort, uint16_t wsPort, uint16_t leaseSeconds);

  // NimBLE callback entry points. These are public only to keep callback
  // adapters allocation-free and live in the implementation file.
  void onCommandRecord(const char* bytes, size_t length);
  void onConnected(bool connected, bool authenticated);

 private:
  bool notifyRecord(const char* record);

  bool running_ = false;
  uint32_t passkey_ = 0;
  char advertisedName_[20] = {};
  char deviceId_[9] = {};
  char pendingRecord_[MAX_RECORD_BYTES + 1] = {};
  std::atomic<size_t> pendingLength_{0};
  std::atomic<bool> connected_{false};
  std::atomic<bool> authenticated_{false};
  NimBLECharacteristic* eventCharacteristic_ = nullptr;
};

}  // namespace Pocket::NearbySync
