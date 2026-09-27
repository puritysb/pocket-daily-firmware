#pragma once

#include <cstdint>

#include "ContentPresentation.h"
#include "ScreenPresentation.h"

class GfxRenderer;
class MappedInputManager;

namespace PocketDaily {
// The one presentation slot of a Sync session (docs/pocket-screen-present-v1.md):
// a card page (content presentation) or the saved Home / Daily Brief (screen
// presentation). Queuing either kind replaces the other, so only the latest
// request has a receipt. A request is refused while either kind is queued or
// drawing. Owned by the transfer activity; every call except the receipt and
// busy reads holds its RenderLock. No allocation of its own.
class PresentationSlot {
 public:
  Content::ContentPresentation content;
  Screen::ScreenPresentation screen;

  bool enqueueContent(const char* revision, uint32_t generation, GfxRenderer& renderer);
  bool enqueueScreen(Screen::Surface surface, uint32_t generation, GfxRenderer& renderer);
  // Main task after HTTP client cleanup, with the RenderLock held.
  bool preparationPending() const { return content.preparationPending() || screen.preparationPending(); }
  bool service(GfxRenderer& renderer, uint32_t freeHeap, uint32_t largestBlock);
  // Render task: true when a presented view owns the panel.
  bool render(GfxRenderer& renderer, const MappedInputManager& input);
  bool visible() const { return content.visible() || screen.visible(); }
  bool busy() const { return content.busy() || screen.busy(); }
  // Card navigation exists only on a visible card page.
  bool navigationAvailable() const { return content.visible(); }
  bool canCaptureFrame() const { return screen.visible() ? screen.canCaptureFrame() : content.canCaptureFrame(); }
  // Back/Dismiss and activity exit: both kinds, fonts and inputs released.
  void hide(GfxRenderer& renderer);
};
}  // namespace PocketDaily
