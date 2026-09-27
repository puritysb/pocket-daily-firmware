#pragma once

#include <cstdint>

class GfxRenderer;
class Page;

// Draws one laid-out EPUB page to the panel: glyph prewarm, BW frame, status
// bar, BW refresh, then the tiled grayscale anti-aliasing pass. Shared by
// EpubReaderActivity and the host page-turn profiler so both run the same
// pipeline; stage boundaries feed PocketDaily::ReaderPerf.
namespace ReaderPageRenderer {

struct Options {
  int fontId = 0;
  int marginTop = 0;
  int marginLeft = 0;
  bool textAntiAliasing = false;
  // Pages between HALF (ghost-cleanup) refreshes (CrossPointSettings::getRefreshFrequency()).
  int refreshFrequency = 15;
  // Draws the status bar into the BW frame (BW pass only). May be null.
  void (*drawStatusBar)(void* context) = nullptr;
  void* context = nullptr;
  // Text the status bar will draw with `statusFontId` (the chapter title when
  // it is drawn with the reader's SD font). Its glyphs load with the page's
  // glyph set, without joining the page's kerning classes, instead of one SD
  // open per glyph from the font's 8-slot overflow ring on every draw.
  const char* statusText = nullptr;
  int statusFontId = 0;
  // True once the reader has queued another page turn. The anti-aliasing pass is then
  // skipped (before it starts) or cut short (between strips, followed by the controller
  // re-sync): the page is already being left, and its gray overlay would only delay the
  // next one by up to ~400 ms on the X3. A page the reader stays on is unaffected.
  bool (*nextTurnQueued)(void* context) = nullptr;
};

// Host profiling: per-stage wall time in microseconds (optional).
struct Timings {
  uint32_t prewarmUs = 0;
  uint32_t bwRenderUs = 0;
  uint32_t statusBarUs = 0;
  uint32_t bwDisplayUs = 0;
  uint32_t grayUs = 0;
  uint32_t cleanupUs = 0;
  // Called as each interval closes, with the field just updated (profiler I/O attribution).
  void (*onClose)(void* context, uint32_t Timings::* field) = nullptr;
  void* context = nullptr;
};

// `pagesUntilFullRefresh` is the reader's HALF-refresh countdown.
void render(GfxRenderer& renderer, const Page& page, const Options& options, int& pagesUntilFullRefresh,
            Timings* timings = nullptr);

}  // namespace ReaderPageRenderer
