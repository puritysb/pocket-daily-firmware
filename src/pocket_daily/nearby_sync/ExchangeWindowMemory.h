#pragma once

#include "ExchangeWindowPolicy.h"

class GfxRenderer;

namespace Pocket::NearbySync::Window {
// Terminal sleep only; no return to drawing in the current boot.
bool releaseSleepFrame(GfxRenderer& renderer);
// Retry only memory admission after releasing rebuildable caches under the render lock.
// Updates the sampled heap used by RTC diagnostics; all admission floors stay unchanged.
Gate prepareWindowMemory(GateInput& input, const GfxRenderer& renderer);
}  // namespace Pocket::NearbySync::Window
