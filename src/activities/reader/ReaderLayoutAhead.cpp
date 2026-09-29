#include "ReaderLayoutAhead.h"

#include <Epub.h>
#include <Logging.h>
#include <Memory.h>

ReaderRenderSpec SectionLayout::renderSpec() const {
  return {.fontId = fontId,
          .lineCompression = lineCompression,
          .extraParagraphSpacing = extraParagraphSpacing,
          .characterSpacing = characterSpacing,
          .wordSpacingPercent = wordSpacingPercent,
          .paragraphAlignment = paragraphAlignment,
          .viewportWidth = viewportWidth,
          .viewportHeight = viewportHeight,
          .hyphenationEnabled = hyphenationEnabled,
          .embeddedStyle = embeddedStyle,
          .imageRendering = imageRendering,
          .focusReadingEnabled = focusReadingEnabled,
          .bilingualViewMode = bilingualViewMode};
}
bool SectionLayout::load(Section& section) const { return section.loadSectionFile(renderSpec()); }
bool SectionLayout::startBuild(Section& section) const { return section.startBuild(renderSpec()); }

bool ReaderLayoutAhead::step(Section& section) {
  const uint16_t before = section.pageCount;
  const bool ok = section.buildSomeMore(1);
  if (section.pageCount > before) pagesLaidOut_ += section.pageCount - before;
  return ok;
}

ReaderLayoutAhead::Result ReaderLayoutAhead::afterPage(Section* shown, const int shownSpine,
                                                       const SectionLayout& layout, const YieldFn shouldYield,
                                                       const HeapFn largestBlock, void* context) {
  const auto yield = [&]() { return shouldYield && shouldYield(context); };
  const auto heap = [&]() { return largestBlock ? largestBlock(context) : UINT32_MAX; };

  // 1) The rest of the chapter on screen, in one run.
  bool shownCompleted = false;
  while (shown && shown->isBuilding() && !yield()) {
    if (shown->pageCount > 0 && heap() < MIN_FREE_BLOCK) {
      LOG_INF("RLA", "Suspending section build: largest free block %u B", static_cast<unsigned>(heap()));
      shown->suspendBuild();
      return Result::Done;
    }
    if (!step(*shown)) {
      LOG_ERR("RLA", "Section %d build after its page failed", shownSpine);
      return Result::ShownFailed;
    }
    shownCompleted = shown->isBuildComplete();
  }
  const Result shownResult = shownCompleted ? Result::ShownComplete : Result::Done;

  // 2) A neighbour: the next chapter from the last page, the previous one from the first pages.
  if (!shown || shown->isBuilding() || shown->isPartial() || shown->pageCount == 0 || yield()) {
    return shownResult;
  }
  int neighbour = -1;
  if (shown->currentPage >= static_cast<int>(shown->pageCount) - 1 && shownSpine + 1 < epub_->getSpineItemsCount()) {
    neighbour = shownSpine + 1;
  } else if (shown->currentPage < BEHIND_WITHIN_PAGES && shownSpine > 0) {
    neighbour = shownSpine - 1;
  }
  if (neighbour < 0) return shownResult;
  prebuild(neighbour, layout, shouldYield, largestBlock, context);
  return shownResult;
}

void ReaderLayoutAhead::prebuild(const int spine, const SectionLayout& layout, const YieldFn shouldYield,
                                 const HeapFn largestBlock, void* context) {
  const auto yield = [&]() { return shouldYield && shouldYield(context); };
  const auto heap = [&]() { return largestBlock ? largestBlock(context) : UINT32_MAX; };
  if (!(prebuildSpine_ == spine && prebuildLayout_ == layout && (prebuildSettled_ || prebuilt_))) {
    if (heap() < PREBUILD_MIN_FREE_BLOCK) return;
    dropBuilding();
    prebuilt_.reset();
    prebuildSpine_ = spine;
    prebuildLayout_ = layout;
    prebuildSettled_ = true;  // unless a build starts below
    auto candidate = makeUniqueNoThrow<Section>(epub_, spine, renderer_);
    if (!candidate) return;
    if (layout.load(*candidate) && !candidate->isPartial()) {
      prebuilt_ = std::move(candidate);  // already laid out: the turn skips reopening it
      return;
    }
    const size_t spineBytes =
        epub_->getCumulativeSpineItemSize(spine) - (spine > 0 ? epub_->getCumulativeSpineItemSize(spine - 1) : 0);
    if (spineBytes > PREBUILD_MAX_UNINFLATED_BYTES && !candidate->hasHtmlCache()) return;
    if (!layout.startBuild(*candidate)) {
      LOG_ERR("RLA", "Pre-build of section %d could not start; it will build on the turn", spine);
      return;
    }
    prebuilt_ = std::move(candidate);
    prebuildSettled_ = false;
  }
  while (prebuilt_ && prebuilt_->isBuilding() && !yield()) {
    if (heap() < MIN_FREE_BLOCK) {
      LOG_INF("RLA", "Suspending pre-build: largest free block %u B", static_cast<unsigned>(heap()));
      prebuilt_.reset();  // the destructor persists the laid-out pages as a partial
      prebuildSettled_ = true;
      return;
    }
    if (!step(*prebuilt_)) {
      LOG_ERR("RLA", "Pre-build of section %d failed; it will build on the turn", spine);
      prebuilt_.reset();
      prebuildSettled_ = true;
      return;
    }
    if (prebuilt_->isBuildComplete()) prebuildSettled_ = true;
  }
}

std::unique_ptr<Section> ReaderLayoutAhead::adopt(const int spine, const SectionLayout& layout) {
  if (prebuilt_ && prebuildSpine_ == spine && prebuildLayout_ == layout) {
    prebuildSpine_ = -1;
    return std::move(prebuilt_);
  }
  dropBuilding();
  if (prebuilt_ && ((prebuildSpine_ != spine + 1 && prebuildSpine_ != spine - 1) || !(prebuildLayout_ == layout))) {
    prebuilt_.reset();  // a finished pre-build no turn from here can use
    prebuildSpine_ = -1;
  }
  return nullptr;
}

void ReaderLayoutAhead::dropBuilding() {
  if (!prebuilt_ || !prebuilt_->isBuilding()) return;
  prebuilt_->abandonBuild();
  prebuilt_.reset();
  prebuildSpine_ = -1;
  prebuildSettled_ = false;
}

void ReaderLayoutAhead::clear() {
  dropBuilding();
  prebuilt_.reset();
  prebuildSpine_ = -1;
  prebuildSettled_ = false;
}
