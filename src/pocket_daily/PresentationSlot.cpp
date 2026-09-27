#include "PresentationSlot.h"

namespace PocketDaily {

bool PresentationSlot::enqueueContent(const char* revision, const uint32_t generation, GfxRenderer& renderer) {
  if (busy() || !content.enqueue(revision, generation)) return false;
  // The newer request replaces the other kind; its receipt ends here.
  screen.hide(renderer);
  return true;
}

bool PresentationSlot::enqueueScreen(const Screen::Surface surface, const uint32_t generation, GfxRenderer& renderer) {
  if (busy() || !screen.enqueue(surface, generation)) return false;
  content.hide(renderer);
  return true;
}

bool PresentationSlot::service(GfxRenderer& renderer, const uint32_t freeHeap, const uint32_t largestBlock) {
  // At most one kind is pending: queuing either hides the other.
  if (screen.preparationPending()) return screen.service(renderer, freeHeap, largestBlock);
  return content.service(renderer, freeHeap, largestBlock);
}

bool PresentationSlot::render(GfxRenderer& renderer, const MappedInputManager& input) {
  if (screen.visible()) return screen.render(renderer);
  return content.render(renderer, input);
}

void PresentationSlot::hide(GfxRenderer& renderer) {
  content.hide(renderer);
  screen.hide(renderer);
}

}  // namespace PocketDaily
