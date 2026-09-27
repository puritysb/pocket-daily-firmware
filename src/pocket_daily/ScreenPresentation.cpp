#include "ScreenPresentation.h"

#include <GfxRenderer.h>
#include <HalSystem.h>

#include "ContentTextLayout.h"
#include "PocketProfileStore.h"
#include "ScreenFrame.h"
#include "SdCardFontSystem.h"
#include "util/CjkScript.h"

namespace PocketDaily::Screen {
namespace {
using Content::PresentationFailure;
using Content::PresentationPhase;

constexpr char kUiFontFamily[] = "PocketSansWorld";
// Cold admission gates shared with content presentation; not lowered here.
constexpr uint32_t kMinFreeHeap = 16384;
constexpr uint32_t kMinLargestBlock = 4096;
// Regular and bold: the painters draw both and GUI.drawHeader asks for bold.
constexpr uint8_t kStyles = 0x03;
// truncatedText appends U+2026; drawWrappedFixed appends ASCII dots.
constexpr const char* kTruncationMarks[] = {"\xE2\x80\xA6", "..."};

struct CoverageCheck {
  GfxRenderer& renderer;
  int font;
};

// Missing coverage or a latched bounded read failure is an error, never a
// frame of replacement glyphs. Layout whitespace is normalized first.
bool fontCovers(void* context, const char* text) {
  return Content::checkLayoutText(text, context, [](void* value, const char* chunk) {
    const auto& check = *static_cast<CoverageCheck*>(value);
    return check.renderer.ensureSdCardFontReady(check.font, chunk, kStyles) == 0 &&
           sdFontSystem.boundedUiFontReady(check.font);
  });
}
}  // namespace

ScreenPresentation::ScreenPresentation() = default;
ScreenPresentation::~ScreenPresentation() = default;

bool ScreenPresentation::enqueue(const Surface surface, const uint32_t generation) {
  if (busy()) return false;
  switch (surface) {
    case Surface::Home:
    case Surface::Brief:
      break;
    case Surface::None:
      return false;
  }
  requested_ = {};
  requested_.surface = surface;
  requested_.generation = generation;
  frame_.reset();  // a retained frame never outlives its paint; defensive
  preparationPending_ = true;
  failure_ = PresentationFailure::None;
  phase_ = PresentationPhase::Queued;
  drawing_.store(true, std::memory_order_release);
  return true;
}

bool ScreenPresentation::service(GfxRenderer& renderer, const uint32_t freeHeap, const uint32_t largestBlock) {
  if (!preparationPending_) return false;
  preparationPending_ = false;
  requested_.heap = freeHeap;
  requested_.block = largestBlock;
  // Preserve the cold admission budget; only its lifetime moves out of HTTP.
  // Fail once, never spin/retry or change the radio.
  if (freeHeap < kMinFreeHeap || largestBlock < kMinLargestBlock) {
    fail(renderer, PresentationFailure::Memory);
    return false;
  }
  // The profile may have been saved again since the request was queued.
  const auto& profile = DailyProfile::current();
  if (DailyProfile::generation() != requested_.generation ||
      (requested_.surface == Surface::Brief && profile.sleepMode == DailyProfile::SleepMode::Reader)) {
    fail(renderer, PresentationFailure::Preparation);
    return false;
  }
  switch (loadFrame(requested_.surface, profile, frame_)) {
    case LoadResult::Ready:
      break;
    case LoadResult::OutOfMemory:
      fail(renderer, PresentationFailure::Memory);
      return false;
    case LoadResult::Unavailable:
      fail(renderer, PresentationFailure::Preparation);
      return false;
  }
  if (!frame_ || !prepareFont(renderer)) {
    fail(renderer, PresentationFailure::Preparation);
    return false;
  }
  return true;  // still Queued and busy until the render task paints
}

bool ScreenPresentation::prepareFont(GfxRenderer& renderer) {
  font_ = 0;
  // Text without Hangul, Kana or Han uses the built-in flash fonts: load the
  // bounded family only when a drawn string needs it.
  bool needsCjk = false;
  visitFrameText(*frame_, &needsCjk, [](void* context, const char* text) {
    if (!CjkScript::needsCjkFont(text)) return true;
    *static_cast<bool*>(context) = true;
    return false;
  });
  if (!needsCjk) return true;
  font_ = sdFontSystem.ensureUiFamilyLoaded(renderer, kUiFontFamily, SdCardFont::LoadMode::BoundedUI,
                                            HalSystem::feedWatchdogIfRegistered);
  if (!font_) return false;
  // Every drawn string, the header title and the truncation marks, before the
  // framebuffer is cleared.
  CoverageCheck check{renderer, font_};
  bool covered = visitFrameText(*frame_, &check, fontCovers);
  for (const char* mark : kTruncationMarks) covered = covered && fontCovers(&check, mark);
  return covered;
}

bool ScreenPresentation::render(GfxRenderer& renderer) {
  if (!visible()) return false;
  if (preparationPending_) return true;  // incidental paints cannot outrun HTTP cleanup
  // Do not redraw on RSSI/heartbeat changes: a rendered or failed request
  // keeps the panel until a new request or Back.
  if (phase_.load(std::memory_order_acquire) != PresentationPhase::Queued || !frame_) return true;
  // Pocket Daily paints in portrait; restore the transfer view's orientation.
  const auto orientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  bool drawn = drawFrame(renderer, *frame_, font_);
  // A bounded read failure while drawing latches; never certify that frame.
  if (font_ && !sdFontSystem.boundedUiFontReady(font_)) drawn = false;
  renderer.setOrientation(orientation);
  if (!drawn) {
    // Keep the physical panel; the partially drawn buffer is never displayed.
    fail(renderer, PresentationFailure::Display);
    return true;
  }
  renderer.displayBuffer();
  // Release glyph memory and the paint inputs before admitting the next
  // transfer. E-ink keeps the frame.
  release(renderer);
  // This means the existing display-driver call returned, not optical proof.
  phase_ = PresentationPhase::Rendered;
  drawing_.store(false, std::memory_order_release);
  return true;
}

void ScreenPresentation::hide(GfxRenderer& renderer) {
  preparationPending_ = false;
  requested_ = {};
  release(renderer);
  failure_ = PresentationFailure::None;
  phase_ = PresentationPhase::Idle;
  drawing_.store(false, std::memory_order_release);
}

Receipt ScreenPresentation::receipt() const {
  Receipt receipt = requested_;
  receipt.phase = phase_.load(std::memory_order_acquire);
  receipt.failure = failure_.load(std::memory_order_acquire);
  return receipt;
}

void ScreenPresentation::release(GfxRenderer& renderer) {
  if (font_) sdFontSystem.releaseLoaded(renderer);
  font_ = 0;
  frame_.reset();
}

void ScreenPresentation::fail(GfxRenderer& renderer, const PresentationFailure failure) {
  release(renderer);
  failure_ = failure;
  phase_ = PresentationPhase::Failed;
  drawing_.store(false, std::memory_order_release);
}
}  // namespace PocketDaily::Screen
