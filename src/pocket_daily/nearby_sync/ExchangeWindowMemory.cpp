#include "ExchangeWindowMemory.h"

#include <Arduino.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>

#include "activities/RenderLock.h"

namespace Pocket::NearbySync::Window {
bool releaseSleepFrame(GfxRenderer& renderer) {
  RenderLock lock;
  return renderer.releaseFrameBufferForSleep();
}

Gate prepareWindowMemory(GateInput& input, const GfxRenderer& renderer) {
  const Gate gate = evaluate(input);
  if (gate != Gate::LOW_MEMORY) return gate;

  // The completed shell/sleep frame no longer needs its glyph caches. Keep
  // families loaded so the next paint can rebuild them; never evict during paint.
  RenderLock lock;
  if (auto* caches = renderer.getFontCacheManager()) caches->releaseSdFontCaches();
  input.freeHeap = ESP.getFreeHeap();
  input.largestBlock = ESP.getMaxAllocHeap();
  return evaluate(input);
}
}  // namespace Pocket::NearbySync::Window
