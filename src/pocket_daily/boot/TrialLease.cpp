#include "TrialLease.h"

#include <Arduino.h>
#include <BoardConfig.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_ota_ops.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <mbedtls/sha256.h>
#include <nvs.h>

#include <algorithm>
#include <cstring>

#include "TrialReader.h"
#include "network/FirmwareFlasher.h"
#include "network/OtaBootSwitch.h"

// Reuse the product-owned no-init words rather than changing RTC layout.
// A distinct magic is never accepted by the ordinary silent-reboot decoder.
extern uint32_t silentRebootMagic;
extern uint32_t silentRebootTarget;
extern uint32_t silentRebootPayload;

namespace PocketDaily::TrialLease {
namespace {
constexpr char kNamespace[] = "pdtrial";
constexpr char kKey[] = "lease";
Record current;
esp_timer_handle_t deadline = nullptr;
bool trialActive = false;
bool syncRequested = false;
bool deadlineExit = false;

bool save(Record& record) {
  record.checksum = checksum(record);
  nvs_handle_t handle;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
  const bool ok = nvs_set_blob(handle, kKey, &record, sizeof(record)) == ESP_OK && nvs_commit(handle) == ESP_OK;
  nvs_close(handle);
  if (!ok) return false;
  Record actual;
  return read(actual) && std::memcmp(&record, &actual, sizeof(record)) == 0;
}

const esp_partition_t* slot(uint32_t address) {
  const auto subtype = address == kSlot0 ? ESP_PARTITION_SUBTYPE_APP_OTA_0 : ESP_PARTITION_SUBTYPE_APP_OTA_1;
  const auto* part = esp_partition_find_first(ESP_PARTITION_TYPE_APP, subtype, nullptr);
  return part && part->address == address &&
                 ((address == kSlot0 && (part->size == kSlotSize || part->size == kX3SlotSize)) ||
                  (address == kSlot1 && part->size == kSlotSize) || (address == kX3Slot1 && part->size == kX3SlotSize))
             ? part
             : nullptr;
}

// Boot/explicit main-task installation only. Reuse the existing flash staging
// buffer: no additional permanent buffer or transient heap allocation.
bool partitionDigest(const esp_partition_t* part, size_t bytes, uint8_t* actual) {
  if (!part || bytes > part->size) return false;
  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);
  bool ok = mbedtls_sha256_starts(&context, 0) == 0;
  auto* buffer = firmware_flash::sharedStagingBuffer();
  for (size_t offset = 0; ok && offset < bytes; offset += firmware_flash::STAGING_BUFFER_BYTES) {
    const size_t n = std::min(bytes - offset, firmware_flash::STAGING_BUFFER_BYTES);
    ok = esp_partition_read(part, offset, buffer, n) == ESP_OK && mbedtls_sha256_update(&context, buffer, n) == 0;
    feedLoopWDT();
    delay(1);
  }
  ok = ok && mbedtls_sha256_finish(&context, actual) == 0;
  mbedtls_sha256_free(&context);
  return ok;
}
bool hashPartition(const esp_partition_t* part, size_t bytes, const uint8_t* expected) {
  uint8_t actual[32];
  return partitionDigest(part, bytes, actual) && std::memcmp(actual, expected, sizeof(actual)) == 0;
}

#ifdef ENABLE_DEV_REMOTE_FLASH
// Keep the SHA/file working frame separate from the producer control frame.
__attribute__((noinline)) bool hashStagedImage() {
  HalFile file;
  if (!Storage.openFileForRead("TRIAL", "/update.bin", file) ||
      file.size() > layoutBytes(current.returnAddress, current.trialAddress) || file.size() < 65536)
    return false;
  current.imageBytes = file.size();
  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);
  bool ok = mbedtls_sha256_starts(&context, 0) == 0;
  bool capability = false;
  auto* buffer = firmware_flash::sharedStagingBuffer();
  char overlap[sizeof(kCapability)] = {};
  size_t offset = 0;
  while (ok && offset < current.imageBytes) {
    const size_t n = std::min(static_cast<size_t>(current.imageBytes) - offset, firmware_flash::STAGING_BUFFER_BYTES);
    ok = file.read(buffer, n) == static_cast<int>(n) && mbedtls_sha256_update(&context, buffer, n) == 0;
    for (size_t i = 0; i < n; ++i) {
      std::memmove(overlap, overlap + 1, sizeof(overlap) - 1);
      overlap[sizeof(overlap) - 1] = static_cast<char>(buffer[i]);
      capability |= std::memcmp(overlap, kCapability, sizeof(kCapability)) == 0;
    }
    offset += n;
    feedLoopWDT();
    delay(1);
  }
  uint8_t actual[32];
  ok = ok && capability && mbedtls_sha256_finish(&context, actual) == 0 &&
       std::memcmp(actual, current.trialSHA256, sizeof(actual)) == 0;
  mbedtls_sha256_free(&context);
  return ok;
}
#endif

bool recover() {
  syncRequested = true;
  // No SD, network or normal UI is required to select the preserved slot.
  // Bound metadata writes even after an interrupted OTA-selector update.
  const bool budget = current.returnAttempts < 2;
  if (!budget || !hashPartition(slot(current.returnAddress), layoutBytes(current.returnAddress, current.trialAddress),
                                current.returnSHA256)) {
    current.failure = budget ? Failure::ReturnImage : Failure::ReturnBudget;
    current.phase = Phase::Failed;
    save(current);
    syncRequested = true;
    return false;
  }
  if (current.failure == Failure::None && !deadlineExit &&
      !(esp_reset_reason() == ESP_RST_DEEPSLEEP && esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER)) {
    current.failure = Failure::UnexpectedReset;
  }
  ++current.returnAttempts;
  current.phase = Phase::Returning;
  current.resetReason = static_cast<uint32_t>(esp_reset_reason());
  current.wakeCause = static_cast<uint32_t>(esp_sleep_get_wakeup_cause());
  // If recording the return attempt fails, a previously verified return image
  // is still safer than leaving production without a deadline. Selector work
  // occurs once in this call; a selector failure does not cause a reset loop.
  if (!save(current)) current.failure = Failure::RecordCommit;
  if (!ota_boot::switchTo(slot(current.returnAddress))) {
    current.failure = Failure::Selection;
    save(current);
    return false;
  }
  esp_restart();
  return true;
}

void timeout(void*) {
  // Timer callbacks must remain short. Restart only; next startup sees Running
  // and selects the preserved slot on the main task before SD initialization.
  armDeadlineTicket(silentRebootMagic, silentRebootTarget, silentRebootPayload, current.run);
  esp_restart();
}
}  // namespace

bool read(Record& record) {
  nvs_handle_t handle;
  if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) return false;
  size_t bytes = sizeof(record);
  const bool ok = nvs_get_blob(handle, kKey, &record, &bytes) == ESP_OK && bytes == sizeof(record);
  nvs_close(handle);
  return ok && valid(record);
}

void onBoot() {
  trialActive = false;
  syncRequested = false;
  if (!read(current)) return;
  deadlineExit = consumeDeadlineTicket(silentRebootMagic, silentRebootTarget, silentRebootPayload, current.run,
                                       esp_reset_reason() == ESP_RST_SW);
  const auto* running = esp_ota_get_running_partition();
  if (!running || !slot(running->address)) return;
  const auto* preserved = slot(current.returnAddress);
  const auto* candidate = slot(current.trialAddress);
  const uint32_t bytes = layoutBytes(current.returnAddress, current.trialAddress);
  if (!preserved || !candidate || preserved->size != bytes || candidate->size != bytes) return;
  if (running->address == current.returnAddress && pending(current.phase)) {
    const bool original =
        hashPartition(running, layoutBytes(current.returnAddress, current.trialAddress), current.returnSHA256);
    // Prepared means the trial never reached its checked first boot; do not
    // report this as a successful production round trip.
    if (!original) current.failure = Failure::ReturnImage;
    if (current.phase == Phase::Prepared) current.failure = Failure::Unstarted;
    if (current.phase == Phase::Running) current.failure = Failure::UnexpectedReset;
    current.phase = original && current.phase == Phase::Returning ? Phase::Returned : Phase::Failed;
    if (save(current)) syncRequested = true;
    return;
  }
  if (running->address != current.trialAddress || !pending(current.phase)) return;
  syncRequested = true;
  const bool resumeTimer =
      current.phase == Phase::Running && !deadlineExit && TrialReader::beginTimerResume(current.run);
  bool canStart = current.phase == Phase::Prepared || resumeTimer;
#ifdef ENABLE_RTC_PROBE_ARENA
  const bool resumeGPIO = current.phase == Phase::Running && TrialReader::beginGPIOResume(current.run);
  canStart = canStart || resumeGPIO;
#endif
  if (!canStart) {
    recover();
    return;
  }
  if (!hashPartition(slot(current.returnAddress), layoutBytes(current.returnAddress, current.trialAddress),
                     current.returnSHA256)) {
    current.phase = Phase::Failed;
    current.failure = Failure::ReturnImage;
    save(current);
    return;
  }
  if (!hashPartition(running, current.imageBytes, current.trialSHA256)) {
    current.failure = Failure::TrialImage;
    recover();
    return;
  }
  current.phase = Phase::Running;
  if (!save(current)) {
    current.failure = Failure::RecordCommit;
    recover();
    return;
  }
  // The SDK owns this one timer allocation (freed by the imminent restart).
  // It is allocated only for an explicitly provisioned lease, before Wi-Fi;
  // failure returns immediately instead of starting an unbounded trial.
  esp_timer_create_args_t args = {};
  args.callback = timeout;
  args.name = "pd_trial";
  if (esp_timer_create(&args, &deadline) != ESP_OK ||
      esp_timer_start_once(deadline, static_cast<uint64_t>(current.seconds) * 1000000) != ESP_OK) {
    current.failure = Failure::Timer;
    recover();
    return;
  }
  trialActive = true;
#ifdef ENABLE_RTC_PROBE_ARENA
  syncRequested = !resumeGPIO && !resumeTimer;
#else
  syncRequested = !resumeTimer;
#endif
  LOG_INF("TRIAL", "%s: bounded production boot", kCapability);
}

void returnNow() {
  if (trialActive) timeout(nullptr);
}

void beforeSleep() {
  if (trialActive) {
    TrialReader::commitSleep();
    if (!powerManager.armRecoveryTimer(5)) esp_restart();
  }
}

Surface requestedSurface() {
  if (!syncRequested) return Surface::None;
  return trialActive ? current.surface : Surface::Sync;
}
bool wantsSync() { return requestedSurface() == Surface::Sync; }
bool recordSurfaceMemory(uint32_t free, uint32_t largest) {
  if (!trialActive || !capturesUi(current.surface) || !free || !largest) return false;
  current.surfaceFree = free;
  current.surfaceLargest = largest;
  return save(current);
}
void recordHttpMemory(bool ready, uint32_t free, uint32_t largest) {
  if (!trialActive) return;
  if (ready) {
    current.readyFree = free;
    current.readyLargest = largest;
  } else {
    current.preHttpFree = free;
    current.preHttpLargest = largest;
  }
  if (!save(current)) LOG_ERR("TRIAL", "Could not persist radio memory checkpoint");
}
bool active() { return trialActive; }
const Record* runningTrial() { return trialActive ? &current : nullptr; }

#ifdef ENABLE_DEV_REMOTE_FLASH
bool prepare(uint32_t run, uint32_t seconds, const char* imageSHA256, const char* returnSHA256, Surface surface) {
  nvs_handle_t existing;
  const auto opened = nvs_open(kNamespace, NVS_READONLY, &existing);
  if (opened != ESP_ERR_NVS_NOT_FOUND) {
    if (opened != ESP_OK) return false;
    size_t size = 0;
    const auto found = nvs_get_blob(existing, kKey, nullptr, &size);
    nvs_close(existing);
    if (found != ESP_ERR_NVS_NOT_FOUND) return false;  // malformed/completed records also block
  }
  const auto* running = esp_ota_get_running_partition();
  const auto* next = esp_ota_get_next_update_partition(nullptr);
  if (!running || !next || !layoutBytes(running->address, next->address) ||
      running->size != layoutBytes(running->address, next->address) || next->size != running->size ||
      !slot(running->address) || !slot(next->address) || running->address == next->address ||
      (BoardConfig::ACTIVE.board != BoardConfig::Board::XteinkX3 &&
       BoardConfig::ACTIVE.board != BoardConfig::Board::XteinkX3Uc8279 &&
       BoardConfig::ACTIVE.board != BoardConfig::Board::XteinkX4))
    return false;
  current = {};
  current.run = run;
  current.seconds = seconds;
  current.surface = surface;
  current.returnAddress = running->address;
  current.trialAddress = next->address;
  if (!decodeSHA(imageSHA256, current.trialSHA256)) return false;
  if (!run || seconds < 30 || seconds > durationLimit(surface)) return false;
  uint8_t expectedReturn[32];
  return decodeSHA(returnSHA256, expectedReturn) && hashStagedImage() &&
         partitionDigest(running, running->size, current.returnSHA256) &&
         std::memcmp(expectedReturn, current.returnSHA256, sizeof(expectedReturn)) == 0 && save(current);
}

bool verifyDestination() {
  return read(current) && current.phase == Phase::Prepared &&
         hashPartition(slot(current.trialAddress), current.imageBytes, current.trialSHA256);
}
bool cancelPrepared(uint32_t run) {
  if (!read(current) || current.phase != Phase::Prepared || current.run != run) return false;
  current.phase = Phase::Failed;
  return save(current);
}
bool clear(uint32_t run) {
  if (!read(current) || current.run != run || pending(current.phase)) return false;
  nvs_handle_t handle;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
  const bool ok = nvs_erase_key(handle, kKey) == ESP_OK && nvs_commit(handle) == ESP_OK;
  nvs_close(handle);
  return ok;
}
#endif
}  // namespace PocketDaily::TrialLease
