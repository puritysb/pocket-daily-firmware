#pragma once

#include <Epub/Section.h>

#include <cstddef>
#include <cstdint>
#include <memory>

class Epub;
class GfxRenderer;

// Everything a section's layout depends on besides the book (the section file header).
struct SectionLayout {
  int fontId = 0;
  float lineCompression = 0;
  bool extraParagraphSpacing = false;
  uint8_t paragraphAlignment = 0;
  uint16_t viewportWidth = 0;
  uint16_t viewportHeight = 0;
  bool hyphenationEnabled = false;
  bool embeddedStyle = false;
  uint8_t imageRendering = 0;
  bool focusReadingEnabled = false;
  uint8_t bilingualViewMode = 0;
  int8_t characterSpacing = 0;
  uint8_t wordSpacingPercent = 100;
  ReaderRenderSpec renderSpec() const;
  bool operator==(const SectionLayout&) const = default;

  bool load(Section& section) const;
  bool startBuild(Section& section) const;
};

// Layout ahead of the reader (EpubReaderActivity).
//
// After a page is complete the render task calls afterPage(): it lays out the rest of the
// shown chapter and then, from that chapter's last page, the next chapter (from its first
// pages, the previous one), one page per step, until done or until `shouldYield` reports
// input. The forward turn into the next
// chapter takes the section with adopt(). So each page of a chapter is laid out once, the
// turn path only lays out the page it shows when nothing ran ahead, and a turn waits for
// at most one page of layout. X3 telemetry on 41aea867 (laying out from loop() one page
// per tick) showed nearly every turn of a short-chapter book building on its own path.
class ReaderLayoutAhead {
 public:
  using YieldFn = bool (*)(void* context);
  using HeapFn = uint32_t (*)(void* context);  // largest free heap block

  // Build steps pause below this contiguous block (a render could otherwise hit OOM).
  static constexpr uint32_t MIN_FREE_BLOCK = 12 * 1024;
  // Chapter builds need 8 KB blocks (segmented inflate window, ZIP buffers): a pre-build
  // starts only with twice that contiguous.
  static constexpr uint32_t PREBUILD_MIN_FREE_BLOCK = 16 * 1024;
  // Inflating a spine is one uninterruptible step (~0.1-0.3 s per 100 KB on the X3); larger
  // uninflated spines are left to their own turn (popup).
  static constexpr size_t PREBUILD_MAX_UNINFLATED_BYTES = 512 * 1024;
  // Layout behind: on the first pages of a chapter the previous one is laid out too, so a
  // backward turn into it (which must lay it out to its last page) does not wait.
  static constexpr int BEHIND_WITHIN_PAGES = 3;

  enum class Result : uint8_t {
    Done,          // nothing (more) to do, or yielded
    ShownFailed,   // the shown section's build failed: caller records it and resets it
    ShownComplete  // the shown section finished laying out in this call
  };

  ReaderLayoutAhead(std::shared_ptr<Epub> epub, GfxRenderer& renderer) : epub_(std::move(epub)), renderer_(renderer) {}

  // `shown` is the section on screen (spine `shownSpine`, on page `shown->currentPage`).
  Result afterPage(Section* shown, int shownSpine, const SectionLayout& layout, YieldFn shouldYield,
                   HeapFn largestBlock, void* context);

  // The pre-built section for `spine` when it was made with `layout`, else nullptr (and a
  // pre-build no turn from `spine` can use, i.e. not a neighbour of it, is released).
  std::unique_ptr<Section> adopt(int spine, const SectionLayout& layout);

  // Discards a pre-build still laying out (no partial-file write): call before rendering a
  // page of the current chapter, so a page render never overlaps it.
  void dropBuilding();
  void clear();

  // Pages laid out by afterPage() since construction (host tests).
  uint32_t pagesLaidOut() const { return pagesLaidOut_; }

 private:
  bool step(Section& section);  // one page; counts it
  // Lays out `spine` into the pre-build slot until it is done or `shouldYield` reports input.
  void prebuild(int spine, const SectionLayout& layout, YieldFn shouldYield, HeapFn largestBlock, void* context);

  std::shared_ptr<Epub> epub_;
  GfxRenderer& renderer_;
  std::unique_ptr<Section> prebuilt_;
  int prebuildSpine_ = -1;
  SectionLayout prebuildLayout_;
  bool prebuildSettled_ = false;  // prebuildSpine_ needs no further work
  uint32_t pagesLaidOut_ = 0;
};
