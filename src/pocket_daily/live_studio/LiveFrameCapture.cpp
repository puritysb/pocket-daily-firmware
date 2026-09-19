#include "LiveFrameCapture.h"

#include <Arduino.h>
#include <HalStorage.h>

#include <atomic>

#include "DevTrace.h"
#include "LiveStudioEvents.h"
#include "util/ScreenshotUtil.h"

namespace PocketDaily::LiveFrameCapture {

namespace {
std::atomic<bool> gRequested{false};
std::atomic<uint32_t> gMinIntervalMs{LiveStudio::kMinCaptureIntervalMs};
std::atomic<uint32_t> gSeq{0};
std::atomic<uint32_t> gBytes{0};
std::atomic<uint32_t> gLastCaptureMs{0};
std::atomic<bool> gReady{false};
}  // namespace

void setRequested(bool on, uint32_t minIntervalMs) {
  uint32_t clamped = minIntervalMs;
  if (clamped < LiveStudio::kMinCaptureIntervalMs) clamped = LiveStudio::kMinCaptureIntervalMs;
  gMinIntervalMs.store(clamped);
  gRequested.store(on);
}

bool requested() { return gRequested.load(); }
void maybeCapture(const uint8_t* framebuffer, int width, int height) {
  if (!gRequested.load()) return;
  DEV_TRACE(PocketDaily::DevTrace::CAPTURE_ENTER);
  const uint32_t now = millis();
  if (!LiveStudio::shouldCaptureFrameAt(ESP.getFreeHeap(), now, gLastCaptureMs.load(), gMinIntervalMs.load())) {
    return;
  }
  DEV_TRACE(PocketDaily::DevTrace::CAPTURE_WRITE);
  if (ScreenshotUtil::saveFramebufferAsBmp(LIVE_FRAME_PATH, framebuffer, width, height)) {
    gSeq.fetch_add(1);
    gBytes.store(0);
    HalFile file = Storage.open(LIVE_FRAME_PATH);
    if (file && !file.isDirectory()) {
      gBytes.store(static_cast<uint32_t>(file.size()));
      gLastCaptureMs.store(now);
      gReady.store(true);
    }
    if (file) file.close();
    DEV_TRACE(PocketDaily::DevTrace::CAPTURE_DONE, 1);
  } else {
    DEV_TRACE(PocketDaily::DevTrace::CAPTURE_DONE, 0);
  }
}

uint32_t seq() { return gSeq.load(); }
uint32_t bytes() { return gBytes.load(); }

bool consumeReady() { return gReady.exchange(false); }

void clear() {
  gRequested.store(false);
  gReady.store(false);
}

}  // namespace PocketDaily::LiveFrameCapture
