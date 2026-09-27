#include "ReaderPageRenderer.h"

#include <Epub/Page.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <SdCardFont.h>

#include <cstdint>

#include "pocket_daily/ReaderPerf.h"

namespace ReaderPageRenderer {
namespace {
namespace Perf = PocketDaily::ReaderPerf;

// Grayscale strips: at most 80 physical rows each, as few as that allows, and
// no taller than needed for that count (X3: 7 x 76 rows, 7,524 B scratch; X4:
// 6 x 80 rows). The scratch is the tiled pass's only allocation.
constexpr int MAX_STRIP_ROWS = 80;
int stripRowsFor(const int panelRows) {
  const int strips = (panelRows + MAX_STRIP_ROWS - 1) / MAX_STRIP_ROWS;
  return (panelRows + strips - 1) / strips;
}

// Text lines whose BW ink rows are known, for culling them from grayscale strips
// they do not reach. 48 lines cover any page layout; lines past it (and every
// non-text element) are always drawn. 192 B on the render task's stack.
constexpr size_t MAX_CULLED_LINES = 48;
struct InkRows {
  int16_t first[MAX_CULLED_LINES];
  int16_t last[MAX_CULLED_LINES];  // < first: the line drew nothing
  size_t count = 0;
};

// BW render of the page that also records each text line's physical ink rows.
// A grayscale pass draws, per glyph, a subset of the pixels the BW pass draws
// (BW paints every non-white pixel; gray planes only the gray ones), so a line
// whose BW rows miss a strip draws nothing there: skipping it is exact.
void renderBwTrackingInk(GfxRenderer& renderer, const Page& page, const int fontId, const int left, const int top,
                         InkRows& ink) {
  ink.count = 0;
  for (size_t i = 0; i < page.elements.size(); ++i) {
    PageElement& element = *page.elements[i];
    const bool track = i < MAX_CULLED_LINES && element.getTag() == TAG_PageLine;
    if (track) renderer.beginInkRows();
    element.render(renderer, fontId, left, top);
    if (track) {
      int first = 0, last = -1;
      renderer.endInkRows(&first, &last);
      ink.first[i] = static_cast<int16_t>(first);
      ink.last[i] = static_cast<int16_t>(last);
      ink.count = i + 1;
    } else if (i < MAX_CULLED_LINES) {
      ink.first[i] = 0;
      ink.last[i] = INT16_MAX;  // untracked: always drawn
      ink.count = i + 1;
    }
  }
}

// Text grayscale pass for one strip [stripY0, stripY0 + rows).
void renderTextStrip(GfxRenderer& renderer, const Page& page, const int fontId, const int left, const int top,
                     const InkRows& ink, const int stripY0, const int rows) {
  for (size_t i = 0; i < page.elements.size(); ++i) {
    if (i < ink.count && (ink.last[i] < stripY0 || ink.first[i] >= stripY0 + rows)) continue;
    page.elements[i]->render(renderer, fontId, left, top);
  }
}

// Closes one host-profiling interval (no-op on the device path).
class StageClock {
 public:
  explicit StageClock(Timings* timings) : timings_(timings), last_(timings ? micros() : 0) {}
  void close(uint32_t Timings::* field) {
    if (!timings_) return;
    const uint32_t now = micros();
    timings_->*field += now - last_;
    if (timings_->onClose) timings_->onClose(timings_->context, field);
    last_ = micros();
  }

 private:
  Timings* timings_;
  uint32_t last_;
};

void displayWithRefreshCycle(const GfxRenderer& renderer, const int refreshFrequency, int& pagesUntilFullRefresh) {
  if (pagesUntilFullRefresh <= 1) {
    Perf::addFlags(Perf::FLAG_HALF);
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = refreshFrequency;
  } else {
    renderer.displayBuffer();
    pagesUntilFullRefresh--;
  }
}

uint32_t prewarmedGlyphs(const GfxRenderer& renderer) {
  uint32_t glyphs = 0;
  for (const auto& entry : renderer.getSdCardFonts()) glyphs += entry.second->getStats().uniqueGlyphs;
  return glyphs;
}
}  // namespace

void render(GfxRenderer& renderer, const Page& page, const Options& options, int& pagesUntilFullRefresh,
            Timings* timings) {
  StageClock clock(timings);
  const int fontId = options.fontId;
  const int left = options.marginLeft;
  const int top = options.marginTop;

  // Font prewarm: a scan pass records the page's text, then its glyphs load in
  // one sorted batch and stay pinned for every remaining pass of this page.
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  page.render(renderer, fontId, left, top);  // scan pass
  if (options.statusText && *options.statusText) fcm->recordExtraText(options.statusText, options.statusFontId);
  scope.endScanAndPrewarm();
  Perf::noteGlyphs(prewarmedGlyphs(renderer));
  Perf::mark(Perf::STAGE_PREWARM);
  Perf::sampleHeap();
  clock.close(&Timings::prewarmUs);

  const bool pageHasImages = page.hasImages();
  const bool needsTextGrayscale = options.textAntiAliasing;
  const bool needsAnyGrayscale = needsTextGrayscale || pageHasImages;
  if (pageHasImages) Perf::addFlags(Perf::FLAG_IMAGES);
  InkRows ink;
  renderBwTrackingInk(renderer, page, fontId, left, top, ink);
  Perf::mark(Perf::STAGE_BW_RENDER);
  clock.close(&Timings::bwRenderUs);
  if (options.drawStatusBar) options.drawStatusBar(options.context);
  Perf::mark(Perf::STAGE_STATUS);
  clock.close(&Timings::statusBarUs);

  if (pageHasImages) {
    // Double FAST_REFRESH with selective image blanking (pablohc's technique):
    // HALF_REFRESH sets particles too firmly for the grayscale LUT to adjust.
    // Instead, blank only the image area and do two fast refreshes.
    // Step 1: Display page with image area blanked (text appears, image area white)
    // Step 2: Re-render with images and display again (images appear clean)
    int16_t imgX, imgY, imgW, imgH;
    if (page.getImageBoundingBox(imgX, imgY, imgW, imgH)) {
      renderer.fillRect(imgX + left, imgY + top, imgW, imgH, false);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);

      // Re-render page content to restore images into the blanked area
      // Status bar is not re-rendered here to avoid reading stale dynamic values (e.g. battery %)
      page.render(renderer, fontId, left, top);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    } else {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
    // The image's own page is handled above and doesn't count toward the full
    // refresh cadence. But the grayscale pass below leaves gray charge in the
    // image region that a plain fast diff on the *next* page can't clear, so
    // text there ghosts gray (#2190). Force the next ordinary page onto the
    // HALF ghost-cleanup path, which drives every pixel to its target
    // regardless of residue.
    pagesUntilFullRefresh = 1;
  } else {
    displayWithRefreshCycle(renderer, options.refreshFrequency, pagesUntilFullRefresh);
  }
  Perf::mark(Perf::STAGE_BW_REFRESH);
  clock.close(&Timings::bwDisplayUs);

  if (!needsAnyGrayscale) return;
  const auto leaving = [&options]() {
    return options.nextTurnQueued != nullptr && options.nextTurnQueued(options.context);
  };
  if (leaving()) return;  // nothing written to the gray planes yet: no re-sync needed

  // Tiled grayscale: render each plane band-by-band into a small scratch and
  // stream straight to the controller, leaving the BW framebuffer intact so no
  // full-frame storeBwBuffer is needed; controller RAM is re-synced from the
  // live framebuffer afterward. The page is re-rendered ceil(H/STRIP_ROWS) times
  // per plane, but renderCharImpl culls out-of-band glyphs before decode so the
  // cost stays close to one render. Both text (drawPixel) and images
  // (DirectPixelWriter) honor the active strip target.
  if (renderer.supportsStripGrayscale()) {
    const int gh = renderer.getDisplayHeight();
    const int gwBytes = renderer.getDisplayWidthBytes();
    const int stripRows = stripRowsFor(gh);

    auto scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * stripRows);
    if (!scratch) {
      LOG_ERR("RPR", "OOM: grayscale strip scratch (%d bytes); skipping AA this page", gwBytes * stripRows);
      return;
    }
    Perf::sampleHeap();
    // Bands may be streamed in any order: X4 windows each via setRamArea, X3 via PTL.
    bool cut = false;
    int written = 0;
    for (const bool lsbPlane : {true, false}) {
      renderer.setRenderMode(lsbPlane ? GfxRenderer::GRAYSCALE_LSB : GfxRenderer::GRAYSCALE_MSB);
      for (int y = 0; y < gh && !cut; y += stripRows) {
        if (leaving()) {
          cut = true;
          break;
        }
        const int rows = (gh - y < stripRows) ? (gh - y) : stripRows;
        renderer.beginStripTarget(scratch.get(), y, rows);
        renderer.clearScreen(0x00);
        if (needsTextGrayscale) {
          renderTextStrip(renderer, page, fontId, left, top, ink, y, rows);
        } else {
          page.renderImages(renderer, fontId, left, top);
        }
        renderer.endStripTarget();
        renderer.writeGrayscalePlaneStrip(lsbPlane, scratch.get(), y, rows);
        written++;
        Perf::noteStrip();
      }
    }
    renderer.setRenderMode(GfxRenderer::BW);
    Perf::mark(Perf::STAGE_GRAY_RENDER);
    if (!cut) {
      Perf::addFlags(Perf::FLAG_AA);
      renderer.displayGrayBuffer();
    }
    Perf::mark(Perf::STAGE_GRAY_REFRESH);
    clock.close(&Timings::grayUs);

    // BW framebuffer is intact; re-sync controller RAM for the next
    // differential page turn directly from it (unless no strip reached it).
    if (written > 0) renderer.cleanupGrayscaleWithFrameBuffer();
    Perf::mark(Perf::STAGE_GRAY_SYNC);
    clock.close(&Timings::cleanupUs);
    return;
  }

  // Fallback for a controller without strip support: save the BW frame before
  // the grayscale passes overwrite it, restore after.
  Perf::addFlags(Perf::FLAG_AA);
  if (!renderer.storeBwBuffer()) {
    LOG_ERR("RPR", "Failed to store BW buffer for grayscale render; skipping grayscale this page");
    return;
  }
  const auto renderGrayscalePass = [&]() {
    if (needsTextGrayscale) {
      page.render(renderer, fontId, left, top);
    } else {
      page.renderImages(renderer, fontId, left, top);
    }
  };
  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  renderGrayscalePass();
  renderer.copyGrayscaleLsbBuffers();
  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  renderGrayscalePass();
  renderer.copyGrayscaleMsbBuffers();
  Perf::mark(Perf::STAGE_GRAY_RENDER);
  renderer.displayGrayBuffer();
  Perf::mark(Perf::STAGE_GRAY_REFRESH);
  clock.close(&Timings::grayUs);
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.restoreBwBuffer();
  Perf::mark(Perf::STAGE_GRAY_SYNC);
  clock.close(&Timings::cleanupUs);
}

}  // namespace ReaderPageRenderer
