#include "NearbySyncService.h"

#include <Arduino.h>
#include <HalSystem.h>
#include <Logging.h>
#include <NimBLEDevice.h>
#include <esp_system.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

namespace Pocket::NearbySync {
namespace {
Service* activeService = nullptr;

bool validRequestId(const char* value) {
  if (!value || strlen(value) != 8) return false;
  for (size_t i = 0; i < 8; ++i) {
    if (!isxdigit(static_cast<unsigned char>(value[i])) || islower(static_cast<unsigned char>(value[i]))) return false;
  }
  return true;
}

class ServerCallbacks final : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo& info) override {
    HalSystem::setCrashBreadcrumb(info.isAuthenticated() ? "nearby:connected-authenticated"
                                                         : "nearby:connected-awaiting-auth");
    if (activeService) activeService->onConnected(true, info.isAuthenticated() && info.isEncrypted());
  }

  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override {
    HalSystem::setCrashBreadcrumb("nearby:disconnected");
    if (activeService) {
      activeService->onConnected(false, false);
      if (NimBLEDevice::isInitialized()) NimBLEDevice::startAdvertising();
    }
  }

  uint32_t onPassKeyDisplay() override {
    HalSystem::setCrashBreadcrumb("nearby:passkey-display");
    return activeService ? activeService->passkey() : 0;
  }

  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    const bool accepted = info.isAuthenticated() && info.isEncrypted();
    HalSystem::setCrashBreadcrumb(accepted ? "nearby:authentication-complete" : "nearby:authentication-rejected");
    if (activeService) activeService->onConnected(true, accepted);
    if (!accepted && NimBLEDevice::getServer()) NimBLEDevice::getServer()->disconnect(info.getConnHandle());
  }
};

class CommandCallbacks final : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& info) override {
    if (!activeService || !info.isAuthenticated() || !info.isEncrypted()) return;
    const std::string& value = characteristic->getValue();
    activeService->onCommandRecord(value.data(), value.size());
  }
};

ServerCallbacks serverCallbacks;
CommandCallbacks commandCallbacks;
}  // namespace

bool Service::begin(const char* model, const char* firmware) {
  if (running_) return true;

  const uint64_t chipId = ESP.getEfuseMac();
  snprintf(deviceId_, sizeof(deviceId_), "%08lX", static_cast<unsigned long>(chipId & 0xFFFFFFFFUL));
  snprintf(advertisedName_, sizeof(advertisedName_), "Pocket-%.4s", deviceId_ + 4);
  passkey_ = 100000U + (esp_random() % 900000U);

  const uint32_t heapBefore = ESP.getFreeHeap();
  HalSystem::setCrashBreadcrumb("nearby:nimble-init");
  if (!NimBLEDevice::init(advertisedName_)) {
    LOG_ERR("NEARBY", "NimBLE init failed");
    return false;
  }

  activeService = this;
  NimBLEDevice::setMTU(247);
  NimBLEDevice::setPower(3);
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityPasskey(passkey_);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);

  NimBLEServer* server = NimBLEDevice::createServer();
  if (!server) {
    end();
    return false;
  }
  server->setCallbacks(&serverCallbacks, false);

  NimBLEService* service = server->createService(SERVICE_UUID);
  if (!service) {
    end();
    return false;
  }

  NimBLECharacteristic* status = service->createCharacteristic(
      STATUS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN, MAX_RECORD_BYTES);
  NimBLECharacteristic* command = service->createCharacteristic(
      COMMAND_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN,
      MAX_RECORD_BYTES);
  eventCharacteristic_ = service->createCharacteristic(
      EVENT_UUID,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN,
      MAX_RECORD_BYTES);
  if (!status || !command || !eventCharacteristic_) {
    end();
    return false;
  }

  char statusRecord[MAX_RECORD_BYTES + 1];
  const int statusLength =
      snprintf(statusRecord, sizeof(statusRecord), "V=1;MODEL=%s;ID=%s;FW=%s;CAP=AP,HTTP,SD,COMMIT1",
               model ? model : "X4", deviceId_, firmware ? firmware : "unknown");
  if (statusLength <= 0 || static_cast<size_t>(statusLength) > MAX_RECORD_BYTES) {
    end();
    return false;
  }
  status->setValue(reinterpret_cast<const uint8_t*>(statusRecord), static_cast<size_t>(statusLength));
  command->setCallbacks(&commandCallbacks);
  server->start();

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->setName(advertisedName_);
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->enableScanResponse(true);
  if (!advertising->start(120000)) {
    end();
    return false;
  }

  running_ = true;
  HalSystem::setCrashBreadcrumb("nearby:advertising");
  connected_.store(false, std::memory_order_release);
  authenticated_.store(false, std::memory_order_release);
  LOG_INF("NEARBY", "started name=%s heap=%lu->%lu", advertisedName_, static_cast<unsigned long>(heapBefore),
          static_cast<unsigned long>(ESP.getFreeHeap()));
  return true;
}

void Service::end() {
  HalSystem::setCrashBreadcrumb("nearby:shutdown");
  pendingLength_.store(0, std::memory_order_release);
  connected_.store(false, std::memory_order_release);
  authenticated_.store(false, std::memory_order_release);
  eventCharacteristic_ = nullptr;
  activeService = nullptr;
  if (NimBLEDevice::isInitialized()) {
    NimBLEDevice::stopAdvertising();
    // The Wi-Fi bulk-transfer phase starts immediately after BLE. Delete the
    // server and advertising objects as well as stopping the controller so the
    // scarce internal heap is returned before TCP buffers are allocated.
    if (!NimBLEDevice::deinit(true)) LOG_ERR("NEARBY", "NimBLE deinit failed");
  }
  running_ = false;
  LOG_INF("NEARBY", "stopped heap=%lu", static_cast<unsigned long>(ESP.getFreeHeap()));
}

void Service::onConnected(const bool connected, const bool authenticated) {
  connected_.store(connected, std::memory_order_release);
  authenticated_.store(connected && authenticated, std::memory_order_release);
}

void Service::onCommandRecord(const char* bytes, const size_t length) {
  if (!bytes || length == 0 || length > MAX_RECORD_BYTES || pendingLength_.load(std::memory_order_acquire) != 0) return;
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(bytes[i]);
    if (c < 0x20 || c > 0x7E) return;
  }
  memcpy(pendingRecord_, bytes, length);
  pendingRecord_[length] = '\0';
  pendingLength_.store(length, std::memory_order_release);
}

bool Service::takeCommand(Command& command) {
  const size_t length = pendingLength_.load(std::memory_order_acquire);
  if (length == 0 || length > MAX_RECORD_BYTES) return false;

  // Keep the slot marked occupied until the callback-owned buffer has been
  // copied. Clearing it first would let the NimBLE task overwrite the record
  // while this activity is parsing it.
  char record[MAX_RECORD_BYTES + 1];
  memcpy(record, pendingRecord_, length + 1);
  pendingLength_.store(0, std::memory_order_release);

  char verb[16] = {};
  char requestId[9] = {};
  char extra = 0;
  if (sscanf(record, "%15s %8s %c", verb, requestId, &extra) != 2 || !validRequestId(requestId)) {
    notifyError("00000000", "BAD_COMMAND");
    return false;
  }

  if (strcmp(verb, "PING") == 0)
    command.type = CommandType::PING;
  else if (strcmp(verb, "START_AP") == 0)
    command.type = CommandType::START_AP;
  else if (strcmp(verb, "CANCEL") == 0)
    command.type = CommandType::CANCEL;
  else {
    notifyError(requestId, "UNKNOWN_COMMAND");
    return false;
  }
  memcpy(command.requestId, requestId, sizeof(command.requestId));
  return true;
}

bool Service::notifyOk(const char* requestId) {
  char record[32];
  snprintf(record, sizeof(record), "OK %s", requestId);
  return notifyRecord(record);
}

bool Service::notifyError(const char* requestId, const char* code) {
  char record[80];
  snprintf(record, sizeof(record), "ERR %s %s", requestId ? requestId : "00000000", code ? code : "INTERNAL");
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

bool Service::notifyRecord(const char* record) {
  if (!record || !eventCharacteristic_ || !isConnected() || !isAuthenticated()) return false;
  const size_t length = strlen(record);
  if (length == 0 || length > MAX_RECORD_BYTES) return false;
  eventCharacteristic_->setValue(reinterpret_cast<const uint8_t*>(record), length);
  return eventCharacteristic_->notify();
}

}  // namespace Pocket::NearbySync
