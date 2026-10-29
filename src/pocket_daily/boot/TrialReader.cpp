#include "TrialReader.h"

#include <Arduino.h>
#include <HalDisplay.h>
#include <HalMemory.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <mbedtls/sha256.h>
#include <nvs.h>

#include "CrossPointSettings.h"
#ifdef ENABLE_RTC_PROBE_ARENA
#include <HalGPIO.h>
#include <esp_sleep.h>
#include <esp_system.h>

#include "RTCProbePolicy.h"
#include "util/ScreenshotUtil.h"
#endif

#include <algorithm>
#include <cstring>

#include "TrialLease.h"
#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"
#include "activities/reader/EpubReaderActivity.h"
#include "network/FirmwareFlasher.h"
#include "pocket_daily/ReaderPerf.h"
#include "util/TrialSnapshot.h"

extern HalDisplay display;
void enterDeepSleep(bool fromTimeout);
namespace PocketDaily::TrialReader {
namespace {
constexpr char NS[] = "pdtrial", KEY[] = "reader";
Record current;
TrialWake::Record wake;
bool timerResume = false;
bool saveWake() {
  wake.checksum = TrialWake::checksum(wake);
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
  const bool ok = nvs_set_blob(h, "readerwake", &wake, sizeof(wake)) == ESP_OK && nvs_commit(h) == ESP_OK;
  nvs_close(h);
  TrialWake::Record actual;
  return ok && readWake(actual) && memcmp(&actual, &wake, sizeof(wake)) == 0;
}
char fixture[64]{}, missing[80]{};
bool running = false, navigationPending = false;
uint32_t lastAt = 0, lastRender = 0;
// One 4-byte cursor must survive render callbacks to reject repeated evidence.
uint32_t lastPerfTurn = 0;
#ifdef ENABLE_RTC_PROBE_ARENA
bool observingGPIOWake = false;
volatile uint32_t* probeTrace() { return reinterpret_cast<volatile uint32_t*>(RTCProbe::TRACE_ADDRESS); }
#endif

bool save() {
  current.checksum = checksum(current);
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
  const bool ok = nvs_set_blob(h, KEY, &current, sizeof(current)) == ESP_OK && nvs_commit(h) == ESP_OK;
  nvs_close(h);
  Record actual;
  return ok && read(actual) && memcmp(&actual, &current, sizeof(actual)) == 0;
}
void paths(uint32_t run) {
  snprintf(fixture, sizeof(fixture), "/Pocket trial %lu.epub", static_cast<unsigned long>(run));
  snprintf(missing, sizeof(missing), "%s.missing", fixture);
}
__attribute__((noinline)) bool verifyFixture() {
  HalFile f = Storage.open(fixture);
  if (!f || f.isDirectory() || f.size() != current.bytes) return false;
  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);
  bool ok = mbedtls_sha256_starts(&context, 0) == 0;
  auto* buffer = firmware_flash::sharedStagingBuffer();
  size_t remaining = current.bytes;
  while (ok && remaining) {
    const size_t n = std::min(remaining, firmware_flash::STAGING_BUFFER_BYTES);
    ok = f.read(buffer, n) == static_cast<int>(n) && mbedtls_sha256_update(&context, buffer, n) == 0;
    remaining -= n;
    feedLoopWDT();
    delay(1);
  }
  uint8_t actual[32];
  ok = ok && mbedtls_sha256_finish(&context, actual) == 0 && memcmp(actual, current.sourceSHA256, 32) == 0;
  mbedtls_sha256_free(&context);
  return ok;
}
bool renameOwned(const char* from, const char* to) {
  if (Storage.exists(to)) return false;
  HalFile file = Storage.open(from);
  return file && file.rename(to);
}
void fail() {
  // A bounded fault can leave only this run's file renamed. Restore before
  // returning; never replace an existing path or touch another book's cache.
  if (Storage.exists(missing) && !Storage.exists(fixture)) renameOwned(missing, fixture);
  current.stage = Stage::Failed;
  save();
  running = false;
  TrialLease::returnNow();
}
__attribute__((noinline)) bool frameDigest(uint8_t* out) {
  const auto* bytes = display.getFrameBuffer();
  const auto size = display.getBufferSize();
  return bytes && size && mbedtls_sha256(bytes, size, out, 0) == 0;
}
__attribute__((noinline)) void observeMemory() {
  const auto heap = HalMemory::getInternalHeap();
  current.minFreeHeap = std::min(current.minFreeHeap, static_cast<uint32_t>(heap.freeBytes));
  current.minLargestBlock = std::min(current.minLargestBlock, static_cast<uint32_t>(heap.largestBlockBytes));
  PocketDaily::ReaderPerf::TurnRecord turn;
  // Includes the renderer's existing in-turn heap samples, not just page-ready RAM.
  if (PocketDaily::ReaderPerf::copyNewestAfter(lastPerfTurn, turn) && turn.freeHeap && turn.largestBlock) {
    ++current.renderMemorySamples;
    current.minFreeHeap = std::min(current.minFreeHeap, turn.freeHeap);
    current.minLargestBlock = std::min(current.minLargestBlock, turn.largestBlock);
  }
  ++current.memorySamples;
}
__attribute__((noinline)) bool rememberPosition(EpubReaderActivity& reader) {
  const auto pos = reader.getCurrentPosition();
  current.visibleOffset = pos.visibleTextOffset;
  current.hasVisibleOffset = pos.hasVisibleTextOffset;
  return frameDigest(current.frameSHA256) && TrialSnapshot::save(current.run, display);
}
__attribute__((noinline)) bool samePosition(EpubReaderActivity& reader, int32_t spine, int32_t page) {
  const auto pos = reader.getCurrentPosition();
  return matches(current, spine, page, pos.hasVisibleTextOffset, pos.visibleTextOffset);
}
#ifdef ENABLE_RTC_PROBE_ARENA
__attribute__((noinline)) bool saveGPIOFrame() {
  char path[80];
  snprintf(path, sizeof(path), "/.pocket/rtc-wake-%lu.bmp", static_cast<unsigned long>(current.run));
  if (Storage.exists(path) || !Storage.ensureDirectoryExists("/.pocket")) return false;
  return ScreenshotUtil::saveFramebufferAsBmp(path, display.getFrameBuffer(), display.getDisplayWidth(),
                                              display.getDisplayHeight());
}
__attribute__((noinline)) bool observeGPIOFrame(EpubReaderActivity& reader, std::string_view path) {
  if (path != fixture) return false;
  RenderLock lock(RenderLock::Mode::Try);
  if (!lock.ownsLock()) return false;
  const auto info = reader.getScreenshotInfo();
  if (!info.pageRendered || info.readerType != ScreenshotInfo::ReaderType::Epub) return false;
  auto* trace = probeTrace();
  trace[9] = 1;
  trace[10] = samePosition(reader, info.spineIndex, info.currentPage);
  trace[11] = trace[10] && saveGPIOFrame();
  trace[25] = info.spineIndex;
  trace[26] = info.currentPage;
  trace[27] = display.getBufferSize();
  trace[28] = current.visibleOffset;
  __sync_synchronize();
  trace[2] = trace[7] && trace[8] && trace[9] && trace[10] && trace[11] ? 3 : 4;
  return true;
}
#endif
}  // namespace
bool read(Record& r) {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
  size_t n = sizeof(r);
  const bool ok = nvs_get_blob(h, KEY, &r, &n) == ESP_OK && n == sizeof(r);
  nvs_close(h);
  return ok && valid(r);
}
#ifdef ENABLE_RTC_PROBE_ARENA
bool beginGPIOResume(uint32_t run) {
  if (!read(current) || current.run != run || current.stage != Stage::Sleeping || !readyForSleep(current)) return false;
  auto* trace = probeTrace();
  if (!RTCProbe::permitsGPIOResume(trace, run, esp_reset_reason() == ESP_RST_DEEPSLEEP,
                                   esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO,
                                   esp_sleep_get_gpio_wakeup_status()))
    return false;
  paths(run);
  observingGPIOWake = true;
  trace[23] = static_cast<uint32_t>(esp_reset_reason());
  trace[24] = static_cast<uint32_t>(esp_sleep_get_wakeup_cause());
  // Consume once before any later boot work. A crash or second GPIO wake may
  // return to the bridge but cannot extend the trial indefinitely.
  __sync_synchronize();
  trace[2] = 5;
  LOG_INF("TRIAL", "POCKET_GPIO_WAKE_PROBE_V2: observe unchanged native boot route");
  return true;
}
void releaseGPIOProbe(bool nativePowerReason, bool holdVerified) {
  TrialLease::Record lease;
  if (!TrialLease::read(lease) || !read(current) || current.run != lease.run || current.stage != Stage::Sleeping ||
      !RTCProbe::owned(probeTrace(), current.run) || probeTrace()[2] == 3)
    return;
  auto* trace = probeTrace();
  if (nativePowerReason && observingGPIOWake) {
    trace[7] = 1;
    trace[29] = holdVerified;
    trace[30] = TrialLease::wantsSync();
  }
  // Also runs in the preserved bridge after a rejected sleep or timeout.
  // Cleanup never asserts the line or relabels the actual wake cause.
  if (gpio.releaseProbePowerButton())
    trace[8] = 1;
  else
    LOG_ERR("TRIAL", "Owned GPIO probe actuator release failed");
}
#endif
bool readWake(TrialWake::Record& r) {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
  size_t n = sizeof(r);
  const bool ok = nvs_get_blob(h, "readerwake", &r, &n) == ESP_OK && n == sizeof(r);
  nvs_close(h);
  return ok && TrialWake::valid(r);
}
bool beginTimerResume(uint32_t run) {
  if (!readWake(wake) ||
      !TrialWake::permitsResume(wake, run, esp_reset_reason() == ESP_RST_DEEPSLEEP,
                                esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) ||
      !read(current) || current.run != run || current.stage != Stage::Sleeping || !readyForSleep(current))
    return false;
  wake.stage = TrialWake::Stage::ObservingResume;
  wake.resetReason = static_cast<uint32_t>(esp_reset_reason());
  wake.wakeCause = static_cast<uint32_t>(esp_sleep_get_wakeup_cause());
  if (!saveWake()) return false;  // Consume before normal boot: no second resume after any fault.
  paths(run);
  timerResume = true;
  lastRender = activityManager.renderCount();
  LOG_INF("TRIAL", "POCKET_TRIAL_AUTOWAKE_V1: native timer resume observation");
  return true;
}
bool resuming() { return timerResume; }
bool preparingAutoWake() {
  return running && wake.run == current.run && wake.stage == TrialWake::Stage::WaitingIdle &&
         (current.stage != Stage::Sleeping || !readyForSleep(current));
}
void observeAutoSleep(uint32_t timeoutMs, uint32_t idleMs) {
  if (!running || wake.run != current.run || wake.stage != TrialWake::Stage::WaitingIdle) return;
  wake.timeoutMs = timeoutMs;
  wake.idleMs = idleMs;
  if (!TrialWake::permitsIdleObservation(
          activityManager.isReaderActivity() && current.stage == Stage::Sleeping && readyForSleep(current), timeoutMs,
          idleMs)) {
    fail();
    return;
  }
  wake.stage = TrialWake::Stage::SleepCommitted;
  if (!saveWake()) fail();
}
void commitSleep() {
  if (!running || wake.run != current.run) return;
  if (wake.stage != TrialWake::Stage::SleepCommitted) {
    fail();
    return;
  }
  running = false;
}
bool begin() {
  if (timerResume) return false;  // Observe native boot dispatch, never goToReader here.
#ifdef ENABLE_RTC_PROBE_ARENA
  if (observingGPIOWake) return false;  // Native boot, not a harness goToReader.
#endif
  const auto* lease = TrialLease::runningTrial();
  if (!lease || !read(current) || current.run != lease->run || current.stage != Stage::Prepared) return false;
  paths(current.run);
  running = true;
  lastPerfTurn = 0;
  if (!verifyFixture()) {
    fail();
    return true;
  }
  current.stage = Stage::Reading;
  if (!save()) {
    fail();
    return true;
  }
  if (readWake(wake) && wake.run == current.run && wake.stage == TrialWake::Stage::Prepared) {
    // RAM-only test settings; normal persisted settings load again on wake.
    SETTINGS.sleepTimeoutMinutes = 1;
    SETTINGS.quickResumeSleepScreen = CrossPointSettings::QUICK_RESUME_AFTER_TIMEOUT;
    wake.stage = TrialWake::Stage::WaitingIdle;
    if (!saveWake()) {
      fail();
      return true;
    }
  }
  LOG_INF("TRIAL", "POCKET_TRIAL_READER_V1: normal reader plan");
  lastAt = millis();
  lastRender = activityManager.renderCount();
  activityManager.goToReader(fixture);
  return true;
}
void loadFailed(std::string_view path) {
  if (running && path == fixture && current.stage == Stage::Missing) {
    current.missingObserved = 1;
    if (!save()) fail();
  }
}
void observe(EpubReaderActivity& reader, std::string_view path) {
  if (timerResume) {
    if (path != fixture || activityManager.renderCount() == lastRender) return;
    bool turn = false;
    {
      RenderLock lock(RenderLock::Mode::Try);
      if (!lock.ownsLock()) return;
      const auto info = reader.getScreenshotInfo();
      if (!info.pageRendered || info.readerType != ScreenshotInfo::ReaderType::Epub) return;
      if (wake.stage == TrialWake::Stage::ObservingResume) {
        wake.positionMatched = samePosition(reader, info.spineIndex, info.currentPage);
        wake.stage = wake.positionMatched ? TrialWake::Stage::TurnPending : TrialWake::Stage::Failed;
        turn = wake.positionMatched;
      } else if (wake.stage == TrialWake::Stage::TurnPending) {
        if (!progressed(info.spineIndex, info.currentPage, current.spine, current.page)) return;
        wake.pageAdvanced = 1;
        wake.stage = TrialWake::Stage::Complete;
      }
    }
    if (!saveWake()) {
      timerResume = false;
      TrialLease::returnNow();
      return;
    }
    if (turn) {
      lastRender = activityManager.renderCount();
      if (reader.pageTurn(true)) {
        activityManager.requestUpdate();
        return;
      }
      wake.stage = TrialWake::Stage::Failed;
      saveWake();
    }
    // The explicit trial returns by software reset without reader onExit().
    // Flush both native wake renders so the host can validate this image's
    // fresh tail instead of an older same-version aggregate.
    if (!PocketDaily::ReaderPerf::save()) {
      wake.stage = TrialWake::Stage::Failed;
      saveWake();
    }
    timerResume = false;
    TrialLease::returnNow();
    return;
  }
#ifdef ENABLE_RTC_PROBE_ARENA
  if (observingGPIOWake) {
    if (observeGPIOFrame(reader, path)) {
      observingGPIOWake = false;
      TrialLease::returnNow();
    }
    return;
  }
#endif
  if (!running || path != fixture || millis() - lastAt < 1000 || activityManager.renderCount() == lastRender) return;
  const auto stage = current.stage;
  if (stage != Stage::Reading && stage != Stage::Reopening && stage != Stage::Recovering) return;
  bool turn = false;
  {
    RenderLock lock(RenderLock::Mode::Try);
    if (!lock.ownsLock()) return;
    const auto info = reader.getScreenshotInfo();
    if (!info.pageRendered || info.readerType != ScreenshotInfo::ReaderType::Epub) return;
    if (stage == Stage::Reading) {
      if (!progressed(info.spineIndex, info.currentPage, current.spine, current.page)) return;
      current.spine = info.spineIndex;
      current.page = info.currentPage;
      ++current.rendered;
      observeMemory();
      if (current.rendered == current.requested) {
        if (!rememberPosition(reader)) {
          current.stage = Stage::Failed;
        } else
          current.stage = Stage::Leaving;
      } else
        turn = true;
    } else {
      if (!samePosition(reader, info.spineIndex, info.currentPage))
        current.stage = Stage::Failed;
      else if (stage == Stage::Reopening) {
        current.reopened = 1;
        current.stage = Stage::Missing;
      } else {
        current.recovered = 1;
        current.stage = TrialSnapshot::save(current.run, display, true) ? Stage::Sleeping : Stage::Failed;
      }
    }
  }
  lastAt = millis();
  lastRender = activityManager.renderCount();
  if (turn) {
    if (!reader.pageTurn(true)) {
      fail();
      return;
    }
    activityManager.requestUpdate();
  } else if (!save())
    fail();
}
void loop() {
  if (!running) return;
  switch (current.stage) {
    case Stage::Leaving:
      if (!navigationPending) {
        activityManager.leaveReader();
        navigationPending = true;
      } else if (!activityManager.isReaderActivity()) {
        navigationPending = false;
        current.stage = Stage::Reopening;
        if (!save()) {
          fail();
          return;
        }
        lastRender = activityManager.renderCount();
        activityManager.goToReader(fixture);
      }
      break;
    case Stage::Missing:
      if (!navigationPending) {
        activityManager.leaveReader();
        navigationPending = true;
      } else if (!activityManager.isReaderActivity() && !Storage.exists(missing)) {
        if (!renameOwned(fixture, missing)) {
          fail();
          return;
        }
        lastAt = millis();
        activityManager.goToReader(fixture);  // normal open of deliberately absent owned file
      } else if (current.missingObserved && !activityManager.isReaderActivity()) {
        if (!renameOwned(missing, fixture) || !verifyFixture()) {
          fail();
          return;
        }
        navigationPending = false;
        current.stage = Stage::Recovering;
        if (!save()) {
          fail();
          return;
        }
        lastRender = activityManager.renderCount();
        activityManager.goToReader(fixture);
      }
      break;
    case Stage::Sleeping:
      if (!readyForSleep(current)) {
        fail();
        return;
      }
      if (wake.run == current.run && wake.stage == TrialWake::Stage::WaitingIdle) break;
      // Normal product sleep teardown, retained frame and SD unmount. The
      // existing lease adds only its independently bounded timer return.
      running = false;
      enterDeepSleep(false);
      break;
    case Stage::Failed:
      fail();
      break;
    default:
      break;
  }
}
#ifdef ENABLE_DEV_REMOTE_FLASH
bool prepare(uint32_t run, uint32_t pages, uint32_t bytes, const char* sha, bool autoWake) {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
  size_t n = 0;
  const auto found = nvs_get_blob(h, KEY, nullptr, &n);
  const auto wakeFound = nvs_get_blob(h, "readerwake", nullptr, &n);
  nvs_close(h);
  if (found != ESP_ERR_NVS_NOT_FOUND || wakeFound != ESP_ERR_NVS_NOT_FOUND) return false;
  current = {};
  current.run = run;
  current.requested = pages;
  current.bytes = bytes;
  current.checksum = checksum(current);
  if (!valid(current) || !TrialLease::decodeSHA(sha, current.sourceSHA256)) return false;
  paths(run);
  if (Storage.exists(missing) || !verifyFixture() || !save()) return false;
  if (!autoWake) return true;
  wake = {};
  wake.run = run;
  return saveWake();
}
bool clear(uint32_t run) {
  if (!read(current) || current.run != run) return false;
  TrialLease::Record lease;
  if (current.stage != Stage::Prepared &&
      (!TrialLease::read(lease) || lease.run != run || TrialLease::pending(lease.phase)))
    return false;
  paths(run);
  if (Storage.exists(missing) && (!renameOwned(missing, fixture) || !verifyFixture())) return false;
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
  const auto wakeErased = nvs_erase_key(h, "readerwake");
  const bool ok = (wakeErased == ESP_OK || wakeErased == ESP_ERR_NVS_NOT_FOUND) && nvs_erase_key(h, KEY) == ESP_OK &&
                  nvs_commit(h) == ESP_OK;
  nvs_close(h);
  return ok;
}
#endif
}  // namespace PocketDaily::TrialReader
