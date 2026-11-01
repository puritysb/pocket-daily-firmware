#include "KeyboardRemote.h"

#include <Arduino.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalSystem.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>

#include "ExchangeWindow.h"
#include "KeyboardRemotePolicy.h"
#include "NearbySyncService.h"
#include "activities/RenderLock.h"

namespace Pocket::KeyboardRemote {
namespace {
using namespace NearbySync;
enum class Startup : uint8_t { IDLE, RUNNING, READY, FAILED };
struct Runtime {
  Service service;
  std::unique_ptr<uint8_t[]> strip;
  Window::UnbondedGrace grace;
  uint32_t highest = 0;
  uint32_t session = 0;
  uint32_t began = 0;
  uint32_t lastInput = 0;
  uint32_t turnAt = 0;
  uint32_t frame = 0;
  char request[9] = {};
  char lastReply[80] = {};
  int8_t direction = 0;
  bool awaitingFrame = false;
  bool connected = false;
  bool notificationsReady = false;
};
// Runtime (~1 KiB) exists only during an explicitly enabled session; no
// permanent reserved buffer or reading-session allocation when disabled.
std::unique_ptr<Runtime> runtime;
std::atomic<Startup> startup{Startup::IDLE};
std::atomic<uint32_t> frames{0};
std::atomic<bool> slowRender{false};
std::atomic<uint32_t> minFree{UINT32_MAX}, minBlock{UINT32_MAX}, streamCount{0};
bool armed = false;
bool inputActivity = false;
bool repaintNeeded = false;
GfxRenderer* frameOwner = nullptr;
Failure failure = Failure::None;
Failure lastProblem = Failure::None;
uint32_t armedAt = 0;
uint32_t armedFrame = 0;

void restoreFrame() {
  if (!frameOwner) return;
  std::optional<RenderLock> lock;
  if (!RenderLock::heldByCurrentTask()) lock.emplace();
  if (!frameOwner->restoreFrameBufferAfterRemote()) {
    // No framebuffer means no legal drawing. Recover before another activity
    // runs; never continue with a null framebuffer or relax the reading floor.
    HalSystem::setCrashBreadcrumb("remote:frame-restore-failed");
    ESP.restart();
    for (;;) delay(1000);
  }
  frameOwner = nullptr;
  repaintNeeded = true;
}

bool bonded() {
  nvs_handle_t handle = 0;
  if (nvs_open("nimble_bond", NVS_READONLY, &handle) != ESP_OK) return false;
  bool found = false;
  for (int i = 1; i <= 4 && !found; ++i) {
    char key[16];
    snprintf(key, sizeof(key), "peer_sec_%d", i);
    size_t size = 0;
    found = nvs_get_blob(handle, key, nullptr, &size) == ESP_OK && size > 0;
  }
  nvs_close(handle);
  return found;
}

void startTask(void* context) {
  auto* work = static_cast<Runtime*>(context);
  const bool ok =
      work->service.begin(gpio.deviceIsX3() ? "X3" : "X4", CROSSPOINT_VERSION, Mode::REMOTE, 0, false, work->session);
  startup.store(ok ? Startup::READY : Startup::FAILED, std::memory_order_release);
  vTaskDelete(nullptr);
}

void replyPage() {
  auto& work = *runtime;
  snprintf(work.lastReply, sizeof(work.lastReply), "PAGE %s %lu %lu %lu", work.request,
           static_cast<unsigned long>(millis() - work.turnAt), static_cast<unsigned long>(ESP.getFreeHeap()),
           static_cast<unsigned long>(ESP.getMaxAllocHeap()));
  if (!work.service.notifyRecord(work.lastReply)) return;
  work.awaitingFrame = false;
  work.request[0] = '\0';
}
}  // namespace

void arm() {
  std::optional<RenderLock> lock;
  if (!RenderLock::heldByCurrentTask()) lock.emplace();
  armed = true;
  armedAt = millis();
  minFree.store(UINT32_MAX);
  minBlock.store(UINT32_MAX);
  streamCount.store(0);
  armedFrame = frames.load(std::memory_order_acquire);
  repaintNeeded = true;
  failure = lastProblem = Failure::None;
}
Failure takeFailure() {
  const auto result = failure;
  if (failure != Failure::None) lastProblem = failure;
  failure = Failure::None;
  return result;
}
bool takeRepaint() {
  const bool result = repaintNeeded;
  repaintNeeded = false;
  return result;
}
bool takeActivity() {
  const bool result = inputActivity;
  inputActivity = false;
  return result;
}
bool active() { return runtime && startup.load(std::memory_order_acquire) != Startup::IDLE; }
bool preparing() { return armed; }
bool streaming() {
  return runtime && startup.load(std::memory_order_acquire) == Startup::READY && frameOwner && runtime->awaitingFrame &&
         !armed && frames.load(std::memory_order_acquire) == runtime->frame;
}
bool idle() { return active() && startup.load(std::memory_order_acquire) == Startup::READY && !runtime->request[0]; }
uint8_t* streamBuffer() { return runtime ? runtime->strip.get() : nullptr; }
void requestSlowRender() { slowRender.store(true, std::memory_order_release); }
void recordMemory() {
  const uint32_t free = ESP.getFreeHeap(), block = ESP.getMaxAllocHeap();
  uint32_t value = minFree.load();
  while (free < value && !minFree.compare_exchange_weak(value, free)) {
  }
  value = minBlock.load();
  while (block < value && !minBlock.compare_exchange_weak(value, block)) {
  }
}
void recordStreamingTurn() { streamCount.fetch_add(1); }
uint32_t minimumFree() {
  const auto value = minFree.load();
  return value == UINT32_MAX ? 0 : value;
}
uint32_t minimumBlock() {
  const auto value = minBlock.load();
  return value == UINT32_MAX ? 0 : value;
}
uint32_t streamingTurns() { return streamCount.load(); }
Failure lastFailure() { return failure != Failure::None ? failure : lastProblem; }
void renderFinished() { frames.fetch_add(1, std::memory_order_release); }

void stop() {
  std::optional<RenderLock> lock;
  if (!RenderLock::heldByCurrentTask()) lock.emplace();
  armed = false;
  slowRender.store(false, std::memory_order_release);
  if (!runtime) {
    restoreFrame();
    return;
  }
  const uint32_t began = millis();
  while (startup.load(std::memory_order_acquire) == Startup::RUNNING) {
    if (millis() - began > 15000) {
      HalSystem::setCrashBreadcrumb("remote:start-timeout");
      ESP.restart();  // worker still owns runtime: never free it underneath init
      for (;;) delay(1000);
    }
    delay(10);
  }
  runtime->service.end();
  powerManager.setBleClockRequired(false);
  powerManager.setPowerSaving(false);
  runtime.reset();
  startup.store(Startup::IDLE, std::memory_order_release);
  restoreFrame();
}

void pauseForLayout() {
  std::optional<RenderLock> lock;
  if (!RenderLock::heldByCurrentTask()) lock.emplace();
  if (!runtime || !runtime->request[0] || startup.load(std::memory_order_acquire) != Startup::READY) {
    stop();
    return;
  }
  char record[40];
  snprintf(record, sizeof(record), "LAYOUT %s %08lX", runtime->request, static_cast<unsigned long>(runtime->session));
  if (!runtime->service.notifyRecord(record)) {
    stop();
    return;
  }
  delay(150);  // allow the announced handoff to leave before shutting down BLE
  runtime->service.end();
  powerManager.setBleClockRequired(false);
  restoreFrame();
  startup.store(Startup::IDLE, std::memory_order_release);
  runtime->connected = runtime->notificationsReady = false;
  runtime->grace = {};
  armed = true;
  armedAt = millis();
}

void loop(GfxRenderer& renderer, const bool pageReady) {
  std::optional<RenderLock> lock;
  if (!RenderLock::heldByCurrentTask()) {
    lock.emplace(RenderLock::Mode::Try);
    if (!lock->ownsLock()) return;
  }
  if (runtime) recordMemory();
  if (slowRender.exchange(false, std::memory_order_acq_rel)) {
    pauseForLayout();
    repaintNeeded = true;
    return;
  }
  if (armed && millis() - armedAt >= 120000) {
    failure = Failure::Memory;
    stop();
  }
  if (armed && startup.load(std::memory_order_acquire) == Startup::IDLE && pageReady) {
    if (!runtime && frames.load(std::memory_order_acquire) == armedFrame) return;
    if (runtime && runtime->awaitingFrame && frames.load(std::memory_order_acquire) == runtime->frame) return;
    armed = false;
    Window::close(Window::CloseReason::RADIO_OWNER);
    if (WiFi.getMode() != WIFI_MODE_NULL) {
      failure = Failure::Busy;
      stop();
      return;
    }
    if (powerManager.getBatteryPercentage() <= 10) {
      failure = Failure::Battery;
      stop();
      return;
    }
    if (!bonded()) {
      failure = Failure::Pairing;
      stop();
      return;
    }
    {
      if (auto* cache = renderer.getFontCacheManager()) cache->releaseSdFontCaches();
      if (!renderer.releaseFrameBufferForRemote()) {
        failure = Failure::Busy;
        stop();
        return;
      }
      frameOwner = &renderer;
    }
    // Keep the existing >50 KiB reading floor after NimBLE's ~70 KiB cost,
    // plus startup stack/headroom; do not trade reading safety for advertising.
    LOG_INF("REMOTE", "start free=%lu block=%lu", static_cast<unsigned long>(ESP.getFreeHeap()),
            static_cast<unsigned long>(ESP.getMaxAllocHeap()));
    if (!memoryAllowed(ESP.getFreeHeap(), ESP.getMaxAllocHeap(), true)) {
      failure = Failure::Memory;
      stop();
      return;
    }
    if (!runtime) runtime = makeUniqueNoThrow<Runtime>();
    if (!runtime) {
      failure = Failure::Memory;
      stop();
      return;
    }
    if (!runtime->strip) {
      // One reusable 4 KiB band per enabled session; never a second full frame
      // or a per-page allocation. Stack would exceed the 256 B frame budget.
      runtime->strip = makeUniqueNoThrow<uint8_t[]>(STREAM_BYTES);
      if (!runtime->strip) {
        LOG_ERR("REMOTE", "Could not allocate strip scratch");
        failure = Failure::Memory;
        stop();
        return;
      }
    }
    if (!runtime->began) {
      runtime->began = runtime->lastInput = millis();
      runtime->session = esp_random();
    }
    powerManager.setBleClockRequired(true);
    powerManager.setPowerSaving(false);
    startup.store(Startup::RUNNING, std::memory_order_release);
    if (xTaskCreate(startTask, "PocketRemote", 6144, runtime.get(), 1, nullptr) != pdPASS) {
      startup.store(Startup::FAILED, std::memory_order_release);
    }
  }
  if (!runtime || startup.load(std::memory_order_acquire) == Startup::RUNNING ||
      startup.load(std::memory_order_acquire) == Startup::IDLE)
    return;
  if (startup.load(std::memory_order_acquire) == Startup::FAILED ||
      !memoryAllowed(ESP.getFreeHeap(), ESP.getMaxAllocHeap(), false) || powerManager.getBatteryPercentage() <= 10 ||
      expired(millis(), runtime->lastInput, runtime->began)) {
    if (!memoryAllowed(ESP.getFreeHeap(), ESP.getMaxAllocHeap(), false))
      failure = Failure::Memory;
    else if (powerManager.getBatteryPercentage() <= 10)
      failure = Failure::Battery;
    else if (startup.load(std::memory_order_acquire) == Startup::FAILED)
      failure = Failure::Startup;
    stop();
    return;
  }
  auto& work = *runtime;
  if (work.connected && !work.service.isConnected()) {
    stop();
    return;
  }
  // A background READ1 client may inspect this shared service and leave.
  // Only REMOTE_READY claims the session; that client disconnect must not disarm it.
  if (work.service.isConnected() && !work.service.isAuthenticated() &&
      work.grace.expired(work.service.connectionGeneration(), work.service.connectedAtMs(), millis())) {
    work.service.disconnect();
  }
  if (work.notificationsReady && work.awaitingFrame && frames.load(std::memory_order_acquire) != work.frame)
    replyPage();
  ParsedCommand command;
  // At most one consumed command per pass: key bursts cannot starve input/render.
  if (!work.service.takeCommand(command)) return;
  if (command.verb == Verb::REMOTE_READY) {
    if (!readyAllowed(strtoul(command.requestId, nullptr, 16), work.highest, work.request[0] != '\0')) {
      work.service.notifyError(command.requestId, "SESSION_CHANGED");
      stop();
      return;
    }
    work.connected = true;
    work.notificationsReady = true;
    work.service.notifyOk(command.requestId);
    return;
  }
  if (command.verb == Verb::CANCEL) {
    work.service.notifyOk(command.requestId);
    stop();
    return;
  }
  if (command.verb != Verb::PAGE_NEXT && command.verb != Verb::PAGE_PREV) {
    work.service.notifyError(command.requestId, "REMOTE_ONLY");
    return;
  }
  const uint32_t sequence = strtoul(command.requestId, nullptr, 16);
  switch (admit(sequence, work.highest, work.request[0] != '\0', pageReady, work.lastReply[0] != '\0')) {
    case Admission::Replay:
      work.service.notifyRecord(work.lastReply);
      return;
    case Admission::Stale:
      work.service.notifyError(command.requestId, "STALE");
      return;
    case Admission::Busy:
      work.service.notifyError(command.requestId, "BUSY");
      return;
    case Admission::Accept:
      break;
  }
  powerManager.setPowerSaving(false);
  inputActivity = true;
  work.highest = sequence;
  memcpy(work.request, command.requestId, sizeof(work.request));
  work.lastReply[0] = '\0';
  work.direction = command.verb == Verb::PAGE_NEXT ? 1 : -1;
  work.turnAt = work.lastInput = millis();
}

int8_t takeTurn() {
  if (!runtime) return 0;
  const int8_t direction = runtime->direction;
  runtime->direction = 0;
  return direction;
}

void turnAccepted(const bool accepted) {
  std::optional<RenderLock> lock;
  if (!RenderLock::heldByCurrentTask()) lock.emplace();
  if (!runtime) return;
  if (!accepted) {
    runtime->service.notifyError(runtime->request, "BOUNDARY");
    runtime->request[0] = '\0';
    return;
  }
  runtime->frame = frames.load(std::memory_order_acquire);
  runtime->awaitingFrame = true;
}
}  // namespace Pocket::KeyboardRemote
