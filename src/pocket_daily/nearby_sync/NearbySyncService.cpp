#include "NearbySyncService.h"

#include <Arduino.h>
#include <HalSystem.h>
#include <Logging.h>
#include <NimBLEDevice.h>
#include <esp_system.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace Pocket::NearbySync {
namespace {
Service* activeService = nullptr;

bool windowMode() { return activeService && activeService->mode() == Mode::WINDOW; }

// Window admission: the peer's identity address (the controller resolves the
// private address of a bonded phone through the IRKs NimBLE restores at sync)
// must belong to a stored bond. An unresolved resolvable private address gets
// a short grace to re-encrypt with its bond (ExchangeWindow enforces it); any
// other unknown peer is dropped before it can start pairing.
bool admitToWindow(NimBLEConnInfo& info) {
  const NimBLEAddress identity = info.getIdAddress();
  return NimBLEDevice::isBonded(identity) || identity.isRpa();
}

class ServerCallbacks final : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, NimBLEConnInfo& info) override {
    if (windowMode() && !admitToWindow(info)) {
      HalSystem::setCrashBreadcrumb("readsync:reject-unbonded");
      server->disconnect(info.getConnHandle());
      return;
    }
    HalSystem::setCrashBreadcrumb(windowMode()             ? "readsync:connected"
                                  : info.isAuthenticated() ? "nearby:connected-authenticated"
                                                           : "nearby:connected-awaiting-auth");
    if (activeService) {
      const bool bondedOk = !windowMode() || info.isBonded();
      activeService->onConnected(true, info.isAuthenticated() && info.isEncrypted() && bondedOk, info.getConnHandle(),
                                 millis());
    }
  }

  void onDisconnect(NimBLEServer*, NimBLEConnInfo& info, int) override {
    HalSystem::setCrashBreadcrumb(windowMode() ? "readsync:disconnected" : "nearby:disconnected");
    if (activeService) {
      activeService->onConnected(false, false, info.getConnHandle(), millis());
      if (NimBLEDevice::isInitialized()) NimBLEDevice::startAdvertising();
    }
  }

  uint32_t onPassKeyDisplay() override {
    // Window mode never displays a passkey (no-input/no-output, see begin()).
    HalSystem::setCrashBreadcrumb(windowMode() ? "readsync:passkey-refused" : "nearby:passkey-display");
    return activeService ? activeService->passkey() : 0;
  }

  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    const bool accepted = info.isAuthenticated() && info.isEncrypted() && (!windowMode() || info.isBonded());
    if (windowMode()) {
      HalSystem::setCrashBreadcrumb(accepted ? "readsync:encrypted-bonded" : "readsync:authentication-rejected");
    } else {
      HalSystem::setCrashBreadcrumb(accepted ? "nearby:authentication-complete" : "nearby:authentication-rejected");
    }
    if (activeService) activeService->onAuthenticated(accepted);
    if (!accepted && NimBLEDevice::getServer()) NimBLEDevice::getServer()->disconnect(info.getConnHandle());
  }
};

class CommandCallbacks final : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& info) override {
    if (!activeService || !info.isAuthenticated() || !info.isEncrypted()) return;
    if (windowMode() && !info.isBonded()) return;
    const std::string& value = characteristic->getValue();
    activeService->onCommandRecord(value.data(), value.size());
  }
};

ServerCallbacks serverCallbacks;
CommandCallbacks commandCallbacks;
}  // namespace

bool Service::begin(const char* model, const char* firmware, const Mode requestedMode, const uint32_t advertiseMs) {
  if (running_) return true;

  mode_ = requestedMode;
  const bool window = requestedMode == Mode::WINDOW;
  const uint64_t chipId = ESP.getEfuseMac();
  snprintf(deviceId_, sizeof(deviceId_), "%08lX", static_cast<unsigned long>(chipId & 0xFFFFFFFFUL));
  snprintf(advertisedName_, sizeof(advertisedName_), "Pocket-%.4s", deviceId_ + 4);
  passkey_ = 100000U + (esp_random() % 900000U);
  queue_.clear();

  const uint32_t heapBefore = ESP.getFreeHeap();
  const uint32_t blockBefore = ESP.getMaxAllocHeap();
  HalSystem::setCrashBreadcrumb(window ? "readsync:nimble-init" : "nearby:nimble-init");
  if (!NimBLEDevice::init(advertisedName_)) {
    LOG_ERR("NEARBY", "NimBLE init failed");
    return false;
  }

  activeService = this;
  NimBLEDevice::setMTU(247);
  NimBLEDevice::setPower(3);
  if (window) {
    // An exchange window never pairs: with no input or output a new peer cannot
    // meet the MITM requirement, and without bonding nothing it negotiates is
    // stored (so a stranger cannot evict the person's bond). Bonded phones
    // re-encrypt with their stored MITM-protected key.
    NimBLEDevice::setSecurityAuth(false, true, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  } else {
    NimBLEDevice::setSecurityAuth(true, true, true);
    NimBLEDevice::setSecurityPasskey(passkey_);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
  }

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
  const size_t statusLength = formatStatus(model, deviceId_, firmware, window, statusRecord, sizeof(statusRecord));
  if (statusLength == 0) {
    end();
    return false;
  }
  status->setValue(reinterpret_cast<const uint8_t*>(statusRecord), statusLength);
  command->setCallbacks(&commandCallbacks);
  server->start();

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->setName(advertisedName_);
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->enableScanResponse(true);
  if (!advertising->start(advertiseMs)) {
    end();
    return false;
  }

  running_ = true;
  HalSystem::setCrashBreadcrumb(window ? "readsync:advertising" : "nearby:advertising");
  connected_.store(false, std::memory_order_release);
  authenticated_.store(false, std::memory_order_release);
  // Heap evidence for the window gate (docs/reading-sync-ble-v1.md, Memory).
  LOG_INF("NEARBY", "started %s name=%s heap=%lu->%lu block=%lu->%lu", window ? "window" : "screen", advertisedName_,
          static_cast<unsigned long>(heapBefore), static_cast<unsigned long>(ESP.getFreeHeap()),
          static_cast<unsigned long>(blockBefore), static_cast<unsigned long>(ESP.getMaxAllocHeap()));
  return true;
}

void Service::end() {
  HalSystem::setCrashBreadcrumb(mode_ == Mode::WINDOW ? "readsync:shutdown" : "nearby:shutdown");
  connected_.store(false, std::memory_order_release);
  authenticated_.store(false, std::memory_order_release);
  eventCharacteristic_ = nullptr;
  activeService = nullptr;
  if (NimBLEDevice::isInitialized()) {
    NimBLEDevice::stopAdvertising();
    // The Wi-Fi bulk-transfer phase (and, after a window, the book the person
    // opens) starts immediately after BLE. Delete the server and advertising
    // objects as well as stopping the controller so the scarce internal heap
    // is returned before anything else allocates.
    if (!NimBLEDevice::deinit(true)) LOG_ERR("NEARBY", "NimBLE deinit failed");
  }
  running_ = false;
  queue_.clear();  // NimBLE is gone: no producer left
  LOG_INF("NEARBY", "stopped heap=%lu block=%lu", static_cast<unsigned long>(ESP.getFreeHeap()),
          static_cast<unsigned long>(ESP.getMaxAllocHeap()));
}

bool Service::notifyRecord(const char* record) {
  if (!record || !eventCharacteristic_ || !isConnected() || !isAuthenticated()) return false;
  const size_t length = strlen(record);
  if (length == 0 || length > MAX_RECORD_BYTES) return false;
  eventCharacteristic_->setValue(reinterpret_cast<const uint8_t*>(record), length);
  return eventCharacteristic_->notify();
}

void Service::disconnect() {
  const uint16_t handle = connHandle_.load(std::memory_order_acquire);
  if (!running_ || handle == 0xFFFF || !NimBLEDevice::getServer()) return;
  NimBLEDevice::getServer()->disconnect(handle);
}

}  // namespace Pocket::NearbySync
