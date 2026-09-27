#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#include "ContentPresentation.h"

class GfxRenderer;

// Home and Daily Brief presentation inside a Sync session
// (docs/pocket-screen-present-v1.md). Structured like ContentPresentation:
// HTTP only queues a few bytes; preparation runs on the main task after HTTP
// client cleanup under the activity's RenderLock; the render task paints once
// and releases everything. No task, server, framebuffer or resident buffer.
namespace PocketDaily::Screen {
enum class Surface : uint8_t { None, Home, Brief };

struct Receipt {
  Surface surface = Surface::None;  // None: no current request
  uint32_t generation = 0;
  Content::PresentationPhase phase = Content::PresentationPhase::Idle;
  Content::PresentationFailure failure = Content::PresentationFailure::None;
  uint32_t heap = 0;  // deferred admission sample, 0 before it
  uint32_t block = 0;
};

struct Frame;  // per-paint inputs (ScreenFrame.h)

// Owned by the transfer activity. Mutations/rendering hold its RenderLock.
// Receipt reads run on the main task without the render lock; rendering
// publishes only the atomic phase/failure/busy flags.
class ScreenPresentation {
 public:
  ScreenPresentation();
  ~ScreenPresentation();
  ScreenPresentation(const ScreenPresentation&) = delete;
  ScreenPresentation& operator=(const ScreenPresentation&) = delete;

  // HTTP callback: records surface/generation only (no SD, font or heap).
  bool enqueue(Surface surface, uint32_t generation);
  // Main task after HTTP client cleanup, with the owner's RenderLock held.
  // True when a frame is ready for the render task.
  bool service(GfxRenderer& renderer, uint32_t freeHeap, uint32_t largestBlock);
  bool preparationPending() const { return preparationPending_; }  // main-task scheduling only
  // Render task with RenderLock. True when this view owns the panel.
  bool render(GfxRenderer& renderer);
  bool visible() const { return phase_.load(std::memory_order_acquire) != Content::PresentationPhase::Idle; }
  bool busy() const { return drawing_.load(std::memory_order_acquire); }
  bool canCaptureFrame() const {
    return phase_.load(std::memory_order_acquire) == Content::PresentationPhase::Rendered;
  }
  void hide(GfxRenderer& renderer);
  Receipt receipt() const;
  // True while per-paint inputs are held (queued or drawing); for tests.
  bool holdsInputs() const { return frame_ != nullptr; }

 private:
  void fail(GfxRenderer& renderer, Content::PresentationFailure failure);
  void release(GfxRenderer& renderer);
  bool prepareFont(GfxRenderer& renderer);

  // One per-paint bundle (ScreenFrame.h), allocated in service() and freed
  // after the paint or on any failure. Never retained while idle.
  std::unique_ptr<Frame> frame_;
  int font_ = 0;  // bounded CJK UI font, 0 when every drawn string is Latin
  // <=24B activity-owned request metadata; retained after failure so a lost
  // POST is recoverable by GET.
  Receipt requested_{};
  static_assert(sizeof(Receipt) <= 24, "Bound deferred screen presentation metadata");
  bool preparationPending_ = false;
  std::atomic<Content::PresentationPhase> phase_{Content::PresentationPhase::Idle};
  std::atomic<Content::PresentationFailure> failure_{Content::PresentationFailure::None};
  std::atomic<bool> drawing_{false};
};

// Plain callbacks keep the HTTP service independent from ActivityManager.
struct ScreenPresentationHost {
  void* self = nullptr;
  bool (*enqueue)(void*, Surface, uint32_t) = nullptr;
  Receipt (*state)(void*) = nullptr;
  bool (*busy)(void*) = nullptr;
};
}  // namespace PocketDaily::Screen
