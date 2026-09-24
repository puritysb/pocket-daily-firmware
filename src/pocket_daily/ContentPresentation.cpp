#include "ContentPresentation.h"

#include <GfxRenderer.h>
#include <HalSystem.h>
#include <I18n.h>

#include <cstring>

#include "ContentTextLayout.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "components/UITheme.h"

namespace PocketDaily::Content {
bool ContentPresentation::enqueue(const char* revision, uint32_t generation) {
  if (busy() || !generation || !revision || strlen(revision) != 64) return false;
  for (unsigned i = 0; i < 64; ++i)
    if (!((revision[i] >= '0' && revision[i] <= '9') || (revision[i] >= 'a' && revision[i] <= 'f'))) return false;
  memcpy(requested_.revision, revision, sizeof(requested_.revision));
  requested_.generation = generation;
  requested_.heap = requested_.block = 0;
  // E-ink retains the previous page; its reloadable card snapshot must not
  // count against the next cold preparation's budget.
  view_.reset();
  preparationPending_ = true;
  failure_ = PresentationFailure::None;
  phase_ = PresentationPhase::Queued;
  drawing_.store(true, std::memory_order_release);
  return true;
}

bool ContentPresentation::service(GfxRenderer& renderer, uint32_t freeHeap, uint32_t largestBlock) {
  if (!preparationPending_) return false;
  preparationPending_ = false;
  requested_.heap = freeHeap;
  requested_.block = largestBlock;
  // Preserve the cold admission budget; only its lifetime moves out of HTTP.
  // Fail once, never spin/retry or change the radio.
  if (freeHeap < 16384 || largestBlock < 4096) {
    failure_ = PresentationFailure::Memory;
    phase_ = PresentationPhase::Failed;
    drawing_.store(false, std::memory_order_release);
    return false;
  }
  if (!prepare(requested_.revision, renderer)) return false;
  if (view_.generation() != requested_.generation) {
    sdFontSystem.releaseLoaded(renderer);
    font_ = 0;
    view_.reset();
    failure_ = PresentationFailure::Preparation;
    phase_ = PresentationPhase::Failed;
    drawing_.store(false, std::memory_order_release);
    return false;
  }
  return true;
}

bool ContentPresentation::prepare(const char* revision, GfxRenderer& renderer) {
  drawing_.store(false, std::memory_order_release);
  phase_ = PresentationPhase::Failed;
  failure_ = PresentationFailure::Preparation;
  const auto result = view_.load(revision, HalSystem::feedWatchdogIfRegistered);
  if (result != ContentViewState::LoadResult::Ready && result != ContentViewState::LoadResult::Empty) return false;
  if (!prepareFont(renderer)) {
    view_.reset();
    return false;
  }
  index_ = 0;
  phase_ = PresentationPhase::Queued;
  failure_ = PresentationFailure::None;
  drawing_.store(true, std::memory_order_release);
  return true;
}

bool ContentPresentation::prepareFont(GfxRenderer& renderer) {
  font_ = sdFontSystem.ensureUiFamilyLoaded(renderer, "PocketSansWorld", SdCardFont::LoadMode::BoundedUI,
                                            HalSystem::feedWatchdogIfRegistered);
  if (!font_) return false;
  const auto fail = [&] {
    sdFontSystem.releaseLoaded(renderer);
    font_ = 0;
    return false;
  };
  // Check all authored fields and the localized controls before painting.
  // Missing coverage is an error, never a successful frame of replacement glyphs.
  const auto ready = [&](const char* text, uint8_t styles) {
    struct Check {
      GfxRenderer& renderer;
      int font;
      uint8_t styles;
    } context{renderer, font_, styles};
    return checkLayoutText(text, &context, [](void* value, const char* chunk) {
      const auto& c = *static_cast<Check*>(value);
      return c.renderer.ensureSdCardFontReady(c.font, chunk, c.styles) == 0 && sdFontSystem.boundedUiFontReady(c.font);
    });
  };
  for (const auto* label :
       {tr(STR_BACK), tr(STR_CONTENT_PREVIOUS), tr(STR_CONTENT_NEXT), tr(STR_POCKET_TITLE), tr(STR_POCKET_EMPTY)})
    if (!ready(label, 3)) return fail();
  if (const auto* cards = view_.cards()) {
    for (uint8_t i = 0; i < cards->count; ++i) {
      const auto& card = cards->cards[i].card;
      if (!ready(card.title, 2) || !ready(card.question, 1) || !ready(card.context, 1)) return fail();
    }
  }
  return true;
}

bool ContentPresentation::render(GfxRenderer& renderer, const MappedInputManager& input) {
  if (!visible()) return false;
  if (preparationPending_) return true;  // incidental paints cannot outrun HTTP cleanup
  // Do not redraw on RSSI/heartbeat changes. Explicit navigation/Apply queues a
  // new frame; the server continues servicing the same session between paints.
  if (phase_ == PresentationPhase::Rendered || phase_ == PresentationPhase::Failed) return true;
  const auto* cards = view_.cards();
  const auto* card = cards && index_ < cards->count ? &cards->cards[index_] : nullptr;
  const auto mapped = input.mapLabels(tr(STR_BACK), "", tr(STR_CONTENT_PREVIOUS), tr(STR_CONTENT_NEXT));
  const char* labels[]{mapped.btn1, mapped.btn2, mapped.btn3, mapped.btn4};
  if (!GUI.drawContentPage(renderer, card, view_.revision(), font_, labels)) {
    failure_ = PresentationFailure::Display;
    phase_ = PresentationPhase::Failed;
    sdFontSystem.releaseLoaded(renderer);
    font_ = 0;
    drawing_.store(false, std::memory_order_release);
    return true;  // keep the physical panel; do not replace it with the transfer UI
  }
  renderer.displayBuffer();
  // Release glyph/font memory before allowing the next transfer. E-ink keeps
  // the frame; the bounded card snapshot alone remains for button navigation.
  sdFontSystem.releaseLoaded(renderer);
  font_ = 0;
  // This means the existing display-driver call returned, not optical proof.
  phase_ = PresentationPhase::Rendered;
  drawing_.store(false, std::memory_order_release);
  return true;
}

bool ContentPresentation::navigate(bool next, GfxRenderer& renderer) {
  const auto* cards = view_.cards();
  if (phase_ != PresentationPhase::Rendered || !cards || cards->count < 2) return false;
  if (!prepareFont(renderer)) {
    failure_ = PresentationFailure::Preparation;
    phase_ = PresentationPhase::Failed;
    return false;
  }
  index_ = next ? (index_ + 1) % cards->count : (index_ + cards->count - 1) % cards->count;
  phase_ = PresentationPhase::Queued;
  drawing_.store(true, std::memory_order_release);
  return true;
}

void ContentPresentation::hide(GfxRenderer& renderer) {
  preparationPending_ = false;
  requested_ = {};
  failure_ = PresentationFailure::None;
  view_.reset();
  if (font_) sdFontSystem.releaseLoaded(renderer);
  font_ = 0;
  phase_ = PresentationPhase::Idle;
  drawing_.store(false, std::memory_order_release);
}

PresentationReceipt ContentPresentation::receipt() const {
  PresentationReceipt receipt;
  memcpy(receipt.revision, view_.revision(), sizeof(receipt.revision));
  receipt.generation = view_.generation();
  if (requested_.generation) {
    memcpy(receipt.revision, requested_.revision, sizeof(receipt.revision));
    receipt.generation = requested_.generation;
    receipt.heap = requested_.heap;
    receipt.block = requested_.block;
  }
  receipt.phase = phase_.load(std::memory_order_acquire);
  receipt.failure = failure_.load(std::memory_order_acquire);
  return receipt;
}
}  // namespace PocketDaily::Content
