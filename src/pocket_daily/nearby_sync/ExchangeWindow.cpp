#include "ExchangeWindow.h"

#include <Arduino.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalSystem.h>
#include <Logging.h>
#include <Memory.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>

#include <atomic>
#include <cstdio>
#include <memory>

#include "CrossPointSettings.h"
#include "NearbySyncService.h"
#include "ReadSyncStats.h"
#include "ReadingSyncSession.h"

namespace Pocket::NearbySync::Window {
namespace {
// The NimBLE startup worker: same stack as the Nearby Sync screen's proven
// PocketBLEStart task. Priority 1 like the loop, so buttons stay responsive.
constexpr uint32_t START_TASK_STACK = 6144;
// Bound on waiting for an in-flight NimBLE init before tearing it down.
constexpr uint32_t START_WAIT_MS = 15000;

enum class StartState : uint8_t { IDLE, RUNNING, SUCCEEDED, FAILED };

// Everything a window needs while it is up (~1.6 KB plus NimBLE's own heap).
struct Runtime {
  Service service;
  ReadingSyncSession session{service};
  const char* model = "X4";
  UnbondedGrace grace;
};

// Survives the restart into a Wi-Fi mode, so /api/status can report it.
RTC_NOINIT_ATTR Stats::Record stats;

Controller controller;
std::unique_ptr<Runtime> runtime;
std::atomic<StartState> startState{StartState::IDLE};

// NimBLE keeps bonds in NVS ("nimble_bond", peer_sec_<n>). Reading the keys
// directly answers "is anyone bonded" without starting the controller.
bool hasStoredBond() {
  nvs_handle_t handle = 0;
  if (nvs_open("nimble_bond", NVS_READONLY, &handle) != ESP_OK) return false;
  bool found = false;
  for (int index = 1; index <= 4 && !found; ++index) {
    char key[16];
    snprintf(key, sizeof(key), "peer_sec_%d", index);
    size_t size = 0;
    found = nvs_get_blob(handle, key, nullptr, &size) == ESP_OK && size > 0;
  }
  nvs_close(handle);
  return found;
}

void startTask(void* context) {
  auto* work = static_cast<Runtime*>(context);
  // Advertise until the window closes it: a sleep trigger can move the
  // deadline, and stopRadio() always ends advertising with NimBLE itself.
  const bool started = work->service.begin(work->model, CROSSPOINT_VERSION, Mode::WINDOW, 0);
  startState.store(started ? StartState::SUCCEEDED : StartState::FAILED, std::memory_order_release);
  vTaskDelete(nullptr);
}

void releaseRuntime() {
  if (!runtime) return;
  runtime->session.reset();
  runtime->service.end();
  runtime.reset();
}

// Stops NimBLE and returns its memory before the caller continues.
void stopRadio() {
  if (startState.load(std::memory_order_acquire) == StartState::RUNNING) {
    const unsigned long waitStarted = millis();
    while (startState.load(std::memory_order_acquire) == StartState::RUNNING &&
           millis() - waitStarted < START_WAIT_MS) {
      delay(10);
    }
    if (startState.load(std::memory_order_acquire) == StartState::RUNNING) {
      // Radio ownership cannot be handed off while init is still running.
      // Recover without freeing worker-owned memory or logging on a starved heap.
      HalSystem::setCrashBreadcrumb("readsync:start-timeout");
      ESP.restart();
      for (;;) delay(1000);
    }
  }
  startState.store(StartState::IDLE, std::memory_order_release);
  uint16_t connections = 0, lists = 0, offers = 0;
  if (runtime) {
    const uint32_t served = runtime->service.connectionCount();
    connections = served > UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(served);
    lists = runtime->session.listsSent();
    offers = runtime->session.offersStored();
  }
  const bool wasOpen = static_cast<bool>(runtime);
  releaseRuntime();
  if (wasOpen) {
    Stats::closed(stats, static_cast<uint8_t>(controller.lastCloseReason()), connections, lists, offers,
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  }
  controller.stopped();
  HalSystem::setCrashBreadcrumb("readsync:window-closed");
}

void start() {
  powerManager.setPowerSaving(false);  // the controller needs the normal CPU clock
  GateInput input;
  input.enabled = SETTINGS.pocketReadingSync != 0;
  input.bonded = input.enabled && hasStoredBond();
  input.batteryPercent = powerManager.getBatteryPercentage();
  input.freeHeap = ESP.getFreeHeap();
  input.largestBlock = ESP.getMaxAllocHeap();
  const Gate gate = evaluate(input);
  Stats::startAttempt(stats, static_cast<uint8_t>(controller.trigger()), static_cast<uint8_t>(gate), input.freeHeap,
                      input.largestBlock, gate == Gate::OPEN);
  if (gate != Gate::OPEN) {
    // Skipped silently for the person; one log line per skip for hardware runs.
    if (gate != Gate::LOW_MEMORY)
      LOG_INF("RSYNC", "window %s skipped: %s (heap %lu, block %lu, battery %u%%)", triggerName(controller.trigger()),
              gateName(gate), static_cast<unsigned long>(input.freeHeap),
              static_cast<unsigned long>(input.largestBlock), static_cast<unsigned>(input.batteryPercent));
    controller.started(false, millis());
    return;
  }

  runtime = makeUniqueNoThrow<Runtime>();
  if (!runtime) {
    LOG_ERR("RSYNC", "OOM: window runtime");
    controller.started(false, millis());
    return;
  }
  runtime->model = gpio.deviceIsX3() ? "X3" : "X4";
  HalSystem::setCrashBreadcrumb("readsync:window-start");
  LOG_INF("RSYNC", "window %s starting (heap %lu, block %lu, battery %u%%)", triggerName(controller.trigger()),
          static_cast<unsigned long>(input.freeHeap), static_cast<unsigned long>(input.largestBlock),
          static_cast<unsigned>(input.batteryPercent));
  startState.store(StartState::RUNNING, std::memory_order_release);
  if (xTaskCreate(startTask, "PocketBLEWin", START_TASK_STACK, runtime.get(), 1, nullptr) != pdPASS) {
    LOG_ERR("RSYNC", "Could not create the window start task");
    startState.store(StartState::IDLE, std::memory_order_release);
    runtime.reset();
    controller.started(false, millis());
  }
}

// STARTING: collect the worker's result.
void finishStart() {
  const StartState state = startState.load(std::memory_order_acquire);
  if (state == StartState::RUNNING) return;
  startState.store(StartState::IDLE, std::memory_order_release);
  if (state != StartState::SUCCEEDED) {
    LOG_ERR("RSYNC", "NimBLE did not start for the window");
    releaseRuntime();
    controller.started(false, millis());
    return;
  }
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t largestBlock = ESP.getMaxAllocHeap();
  Stats::ready(stats, freeHeap, largestBlock, readyAllowed(freeHeap, largestBlock));
  if (!readyAllowed(freeHeap, largestBlock)) {
    controller.started(true, millis());
    controller.close(CloseReason::LOW_MEMORY);
    stopRadio();
    return;
  }
  controller.started(true, millis());
  HalSystem::setCrashBreadcrumb("readsync:window-open");
  LOG_INF("RSYNC", "window open for %lu ms (heap %lu, block %lu)",
          static_cast<unsigned long>(controller.remainingMs(millis())), static_cast<unsigned long>(freeHeap),
          static_cast<unsigned long>(largestBlock));
}

// OPEN: serve the bonded phone.
void serve() {
  Service& service = runtime->service;
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t largestBlock = ESP.getMaxAllocHeap();
  Stats::sample(stats, freeHeap, largestBlock);
  if (!runningAllowed(freeHeap, largestBlock)) {
    close(CloseReason::LOW_MEMORY);
    return;
  }
  // A connection whose private address the controller could not resolve must
  // re-encrypt with a bond within the grace period (NearbySyncService admission).
  if (service.isConnected() && !service.isAuthenticated() &&
      runtime->grace.expired(service.connectionGeneration(), service.connectedAtMs(), millis())) {
    HalSystem::setCrashBreadcrumb("readsync:grace-expired");
    service.disconnect();
  }

  ParsedCommand command;
  while (service.takeCommand(command)) {
    switch (command.verb) {
      case Verb::PING:
      case Verb::CANCEL:
        service.notifyOk(command.requestId);
        break;
      case Verb::START_AP:
        // The hotspot needs the Nearby Sync screen (physical presence).
        service.notifyError(command.requestId, NOT_IN_SYNC);
        break;
      default:
        runtime->session.handle(command);
        break;
    }
  }
  runtime->session.pump();
}
}  // namespace

void arm(const Trigger trigger, const uint32_t renderCount) {
  const Controller::State before = controller.state();
  controller.arm(trigger, millis(), renderCount);
  if (before == Controller::State::OPEN) {
    LOG_DBG("RSYNC", "window extended by %s: %lu ms left", triggerName(trigger),
            static_cast<unsigned long>(controller.remainingMs(millis())));
  }
}

void loop(const bool shellActive, const uint32_t renderCount) {
  switch (controller.tick(millis(), renderCount, shellActive)) {
    case Controller::Action::START:
      start();
      return;
    case Controller::Action::STOP:
      stopRadio();
      return;
    case Controller::Action::NONE:
      break;
  }
  if (controller.state() == Controller::State::STARTING) {
    finishStart();
  } else if (controller.state() == Controller::State::OPEN && runtime) {
    serve();
  }
}

void close(const CloseReason reason) {
  if (controller.close(reason) == Controller::Action::STOP) stopRadio();
}

bool active() { return controller.radioUp(); }

const Stats::Record* statistics() { return Stats::valid(stats) ? &stats : nullptr; }

void runBeforeDeepSleep(const uint32_t renderCount) {
  arm(Trigger::SLEEP, renderCount);
  while (controller.state() != Controller::State::IDLE) {
    gpio.update();
    if (gpio.wasAnyPressed()) {
      // The person wants the reader: stop the radio and let sleep proceed.
      close(CloseReason::BUTTON);
      break;
    }
    loop(true, renderCount);
    delay(10);
  }
}

}  // namespace Pocket::NearbySync::Window
