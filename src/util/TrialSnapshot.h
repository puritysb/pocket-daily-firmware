#pragma once

#include <HalDisplay.h>
#include <HalStorage.h>

#include <cstdint>
#include <cstdio>

#include "ScreenshotUtil.h"

namespace PocketDaily::TrialSnapshot {
// Only image-bound, user-authorized trials call this while holding RenderLock.
// Existing screenshot writer uses a bounded row buffer, never a second frame.
[[gnu::noinline]] inline bool save(uint32_t run, HalDisplay& display, bool cached = false) {
  char path[72];
  snprintf(path, sizeof(path), "/Pocket trial %lu%s.bmp", static_cast<unsigned long>(run), cached ? " cached" : "");
  if (!run || Storage.exists(path)) return false;
  return ScreenshotUtil::saveFramebufferAsBmp(path, display.getFrameBuffer(), display.getDisplayWidth(),
                                              display.getDisplayHeight());
}
}  // namespace PocketDaily::TrialSnapshot
