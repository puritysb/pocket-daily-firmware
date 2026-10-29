#pragma once
#include "LayoutInputs.h"
class GfxRenderer;
namespace PocketDaily::Preview {
// Main task under the owner's RenderLock, after HTTP transfer/busy admission.
// Reads only: no panel paint, font load, settings write or active-slot repair.
bool captureLayoutInputs(const GfxRenderer& renderer, LayoutInputs& out);
}  // namespace PocketDaily::Preview
