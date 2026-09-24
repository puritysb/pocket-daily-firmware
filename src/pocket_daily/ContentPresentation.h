#pragma once

#include <atomic>

#include "ContentViewState.h"

class GfxRenderer;
class MappedInputManager;

namespace PocketDaily::Content {
enum class PresentationPhase : uint8_t { Idle, Queued, Rendered, Failed };
enum class PresentationFailure : uint8_t { None, Memory, Preparation, Display };
struct PresentationReceipt {
  char revision[65]{};
  uint32_t generation = 0;
  PresentationPhase phase = PresentationPhase::Idle;
  PresentationFailure failure = PresentationFailure::None;
  uint32_t heap = 0;
  uint32_t block = 0;
};
// Owned by the transfer activity. Mutations/rendering hold its RenderLock.
// Receipt reads run on the metadata-owning main task without the render lock;
// rendering publishes only atomic phase/busy flags. No internal task, server,
// radio or extra framebuffer. Published content is immutable.
class ContentPresentation {
 public:
  // Queue verified active metadata without snapshot/font allocations in HTTP.
  bool enqueue(const char* revision, uint32_t generation);
  // Main task after HTTP client cleanup, with the owner's RenderLock held.
  bool service(GfxRenderer& renderer, uint32_t freeHeap, uint32_t largestBlock);
  bool preparationPending() const { return preparationPending_; }  // main-task scheduling only
  bool prepare(const char* revision, GfxRenderer& renderer);
  bool render(GfxRenderer& renderer, const MappedInputManager& input);
  bool visible() const { return phase_ != PresentationPhase::Idle; }
  bool busy() const { return drawing_.load(std::memory_order_acquire); }
  bool canCaptureFrame() const { return phase_.load(std::memory_order_acquire) == PresentationPhase::Rendered; }
  bool navigate(bool next, GfxRenderer& renderer);
  void hide(GfxRenderer& renderer);
  PresentationReceipt receipt() const;

 private:
  ContentViewState view_;
  std::atomic<PresentationPhase> phase_{PresentationPhase::Idle};
  uint8_t index_ = 0;
  int font_ = 0;
  std::atomic<bool> drawing_{false};
  bool prepareFont(GfxRenderer& renderer);
  // <=88B activity-owned metadata, not another allocation or frame buffer.
  // Retained on failure so a lost POST is recoverable by revision-bound GET.
  PresentationReceipt requested_{};
  static_assert(sizeof(PresentationReceipt) <= 88, "Bound deferred presentation metadata");
  bool preparationPending_ = false;
  std::atomic<PresentationFailure> failure_{PresentationFailure::None};
};
// Plain callbacks keep the HTTP service independent from ActivityManager.
struct PresentationHost {
  void* self = nullptr;
  bool (*prepare)(void*, const char*, uint32_t) = nullptr;
  PresentationReceipt (*state)(void*) = nullptr;
  bool (*busy)(void*) = nullptr;
};
}  // namespace PocketDaily::Content
