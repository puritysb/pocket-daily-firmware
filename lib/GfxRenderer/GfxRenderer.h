#pragma once

#include <EpdFontFamily.h>
#include <HalDisplay.h>

namespace BidiUtils {
// Paragraph base direction for the Unicode BiDi algorithm (UAX#9).
// AUTO: scan text for first strong directional character (P2/P3 rules)
// LTR:  force left-to-right paragraph embedding level
// RTL:  force right-to-left paragraph embedding level
enum class BidiBaseDir : signed char { AUTO = -1, LTR = 0, RTL = 1 };
}  // namespace BidiUtils

class FontCacheManager;
class SdCardFont;
class TtfEpdFont;

// Supplies the optional glyph-fallback SD font. `load == false` only reports an
// already-loaded font (no SD I/O); the provider owns the font object.
using FallbackFontProviderFn = SdCardFont* (*)(void* context, bool load);

#include <array>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "Bitmap.h"

namespace glyphBitmap {
struct Frame;
}

// Color representation: uint8_t mapped to 4x4 Bayer matrix dithering levels
// 0 = transparent, 1-16 = gray levels (white to black)
enum Color : uint8_t { Clear = 0x00, White = 0x01, LightGray = 0x05, DarkGray = 0x0A, Black = 0x10 };

class GfxRenderer {
 public:
  enum RenderMode { BW, GRAYSCALE_LSB, GRAYSCALE_MSB };

  // Logical screen orientation from the perspective of callers
  enum Orientation {
    Portrait,                  // 480x800 logical coordinates (current default)
    LandscapeClockwise,        // 800x480 logical coordinates, rotated 180° (swap top/bottom)
    PortraitInverted,          // 480x800 logical coordinates, inverted
    LandscapeCounterClockwise  // 800x480 logical coordinates, native panel orientation
  };

 private:
  static constexpr size_t BW_BUFFER_CHUNK_SIZE = 8000;  // 8KB chunks to allow for non-contiguous memory

  HalDisplay& display;
  RenderMode renderMode;
  mutable bool absoluteGrayPlanes = false;
  Orientation orientation;
  bool fadingFix;
  uint8_t* frameBuffer = nullptr;
  uint16_t panelWidth = HalDisplay::DISPLAY_WIDTH;
  uint16_t panelHeight = HalDisplay::DISPLAY_HEIGHT;
  uint16_t panelWidthBytes = HalDisplay::DISPLAY_WIDTH_BYTES;
  uint32_t frameBufferSize = HalDisplay::BUFFER_SIZE;
  std::vector<uint8_t*> bwBufferChunks;
  std::map<int, EpdFontFamily> fontMap;
  // Mutable because ensureSdCardFontReady() is const (called from layout code
  // that holds a const GfxRenderer&) but triggers SD card reads and heap
  // allocation inside the SdCardFont objects. Same pragmatic compromise as
  // fontCacheManager_ below.
  mutable std::map<int, SdCardFont*> sdCardFonts_;
  mutable std::map<int, uint16_t> sdCardFontScales_;  // fontId -> 8.8 fixed point scale (256=1.0x)
  // TTF (vector) fonts: rebuilt per page by ensureSdCardFontReady(). Mutable for
  // the same reason as sdCardFonts_ (const layout path triggers a rebuild).
  mutable std::map<int, TtfEpdFont*> ttfFonts_;

  // Mutable because drawText() is const but needs to delegate scan-mode
  // recording to the (non-const) FontCacheManager. Same pragmatic compromise
  // as before, concentrated in a single pointer instead of four fields.
  mutable FontCacheManager* fontCacheManager_ = nullptr;

  // One-shot refresh promotion (see promoteNextRefresh). Mutable because
  // displayBuffer() is const but must consume the flag.
  mutable bool promotedRefreshPending_ = false;
  mutable HalDisplay::RefreshMode promotedRefresh_ = HalDisplay::FAST_REFRESH;
  // Swap in (and clear) the promoted mode, if one is pending.
  HalDisplay::RefreshMode applyPromotedRefresh(HalDisplay::RefreshMode refreshMode) const;

  // Tiled grayscale strip target. When active, drawPixel()/clearScreen()
  // operate on a caller-owned scratch holding one horizontal band of physical
  // rows [_stripY0, _stripY0 + _stripRows) (panelWidthBytes wide) instead of
  // the shared framebuffer, clipping pixels outside the band. Lets grayscale
  // planes render band-by-band straight to the controller without destroying
  // the BW framebuffer (no storeBwBuffer). Mutable because the render path is
  // const. See beginStripTarget()/endStripTarget().
  mutable uint8_t* _stripBuf = nullptr;
  mutable int _stripY0 = 0;
  mutable int _stripRows = 0;
  mutable bool _stripActive = false;
  mutable int clipLeft_ = 0;
  mutable int clipTop_ = 0;
  mutable int clipRight_ = 32767;
  mutable int clipBottom_ = 32767;

  // CJK UI font fallback map: primary (built-in, Latin-only) UI font id -> a
  // size-matched SD-card font id that carries CJK glyphs. When a string drawn
  // or measured with a mapped primary font contains a CJK codepoint the primary
  // cannot render, the whole string is routed to the mapped fallback so it
  // appears at the same point size as the surrounding UI text. Populated by the
  // app-level SD font setup when an SD family is loaded. See resolveTextFontId().
  std::map<int, int> fallbackFontMap_;

  // If `text` contains a CJK codepoint that `fontId` cannot render and `fontId`
  // has a registered fallback, returns the fallback id; otherwise returns
  // fontId unchanged. The whole string is routed as a unit so each draw/measure
  // call stays single-font (consistent bit depth, metrics, wrapping).
  int resolveTextFontId(int fontId, const char* text, EpdFontFamily::Style style) const;

  // Batch-load `text`'s glyphs into an SD-card font's resident mini tables
  // before a per-glyph measure/draw loop runs. Called when resolveTextFontId
  // redirected a string to the SD fallback: UI screens (file browser, home)
  // draw those strings without the reader's PrewarmScope, and every glyph
  // would otherwise fault through SdCardFont::onGlyphMiss — one .cpfont file
  // open + seek + read per glyph, per redraw, through an 8-slot overflow ring
  // (#2725). One prewarm per string costs a single file open; re-measuring or
  // re-drawing resident glyphs is a RAM-only subset check. No-op for built-in
  // fonts.
  void ensureSdGlyphsResident(int fontId, const char* text, EpdFontFamily::Style style, bool metadataOnly) const;

  // Ink row tracking (see beginInkRows): physical rows of the glyph and line
  // boxes drawn outside strip mode. Mutable for the same reason as the strip target.
  mutable bool _inkTracking = false;
  mutable int _inkMinRow = 0;
  mutable int _inkMaxRow = -1;

  // Optional glyph-fallback font supplied by the SD font system (see
  // setFallbackFontProvider). Called, not cached, so the owner may unload it.
  FallbackFontProviderFn fallbackProvider_ = nullptr;
  void* fallbackContext_ = nullptr;
  uint32_t glyphLayoutKey_ = 0;

 public:
  // A glyph together with the font data that owns its bitmap. `primary` is
  // false for glyphs borrowed from the fallback font.
  struct GlyphRef {
    const EpdGlyph* glyph = nullptr;
    const EpdFontData* data = nullptr;
    bool primary = false;
  };

 private:
  bool fallbackAllowed(int fontId) const;
  // RAM-only coverage check: never loads a glyph or touches SD for Cached SD
  // fonts and flash fonts.
  bool fontCovers(int fontId, const EpdFontFamily& font, uint32_t cp, EpdFontFamily::Style style) const;
  GlyphRef resolveGlyph(const EpdFontFamily& font, uint32_t cp, EpdFontFamily::Style style, bool allowFallback) const;
  int32_t fallbackAdvanceFP(uint32_t cp, EpdFontFamily::Style style, bool allowFallback,
                            const EpdFontData* primaryData) const;
  int prepareFallbackAdvances(int fontId, const std::vector<std::string>* words, const char* text,
                              const std::vector<EpdFontFamily::Style>* wordStyles) const;
  void freeBwBufferChunks();
  template <Color color>
  void drawPixelDither(int x, int y) const;
  template <Color color>
  void fillArc(int maxRadius, int cx, int cy, int xDir, int yDir) const;
  // Byte-aligned, orientation-specialized rectangle fill. Rotates the rect's
  // two opposing corners into physical-framebuffer space once, then walks each
  // physical row with head-mask / middle memset / tail-mask byte writes — no
  // per-pixel rotation, no per-pixel RMW.
  template <Color color>
  void fillRectImpl(int x, int y, int width, int height) const;

 public:
  explicit GfxRenderer(HalDisplay& halDisplay)
      : display(halDisplay), renderMode(BW), orientation(Portrait), fadingFix(false) {}
  ~GfxRenderer() { freeBwBufferChunks(); }

  // Setup
  void begin();  // must be called right after display.begin()
  void insertFont(int fontId, EpdFontFamily font);
  // Clears both the flash-font map and any SD-font registration for fontId.
  // Coupled to avoid dangling SdCardFont* in sdCardFonts_ when callers free
  // the underlying SdCardFont and forget the SD-side unregister.
  void removeFont(int fontId) {
    fontMap.erase(fontId);
    sdCardFonts_.erase(fontId);
    sdCardFontScales_.erase(fontId);
  }
  void setFontCacheManager(FontCacheManager* m);
  FontCacheManager* getFontCacheManager() const { return fontCacheManager_; }
  // Batch-prewarm CJK fallback glyphs for a screenful of static strings in ONE
  // SD pass. List screens redraw every visible row on each repaint; without an
  // up-front batch each row's draw prewarms per-string, and under heap
  // pressure (union merge disabled) each string evicts the previous one — SD
  // reads on every repaint forever. Call once when the screen's strings are
  // known (data load); later measures/draws become RAM-only subset hits.
  // No-op when nothing routes to an SD fallback.
  // The getter form fetches strings one at a time (allocation-free — callers
  // must NOT build a concatenated std::string: its bare-new growth aborts on
  // the heap-tight screens this exists for). A null getter result skips that
  // index.
  using TextGetter = const char* (*)(const void* ctx, uint32_t index);
  void prewarmFallbackText(int fontId, TextGetter getter, const void* ctx, uint32_t textCount,
                           EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  void prewarmFallbackText(int fontId, const char* text, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  bool isFontCacheScanning() const;
  const std::map<int, EpdFontFamily>& getFontMap() const { return fontMap; }
  void registerSdCardFont(int fontId, SdCardFont* font) { sdCardFonts_[fontId] = font; }
  void unregisterSdCardFont(int fontId) { removeFont(fontId); }
  void clearSdCardFonts() {
    sdCardFonts_.clear();
    sdCardFontScales_.clear();
  }
  void registerSdCardFontScale(int fontId, uint16_t scale) { sdCardFontScales_[fontId] = scale; }
  void clearSdCardFontScales() { sdCardFontScales_.clear(); }
  uint16_t getSdCardFontScale(int fontId) const {
    auto it = sdCardFontScales_.find(fontId);
    return (it != sdCardFontScales_.end()) ? it->second : 256;
  }
  const std::map<int, SdCardFont*>& getSdCardFonts() const { return sdCardFonts_; }
  bool isSdCardFont(int fontId) const { return sdCardFonts_.count(fontId) > 0; }
  // TTF (vector) fonts rendered via TtfEpdFont/FreeInkFont. Registered like an
  // ordinary EpdFontFamily (insertFont), plus tracked here so ensureSdCardFontReady()
  // rebuilds their per-page glyph set on demand — the eager analogue of the SD
  // font prewarm. The TtfEpdFont is owned by the caller (SdCardFontSystem).
  void registerTtfFont(int fontId, TtfEpdFont* font) { ttfFonts_[fontId] = font; }
  void unregisterTtfFont(int fontId) { ttfFonts_.erase(fontId); }
  const std::map<int, TtfEpdFont*>& getTtfFonts() const { return ttfFonts_; }
  // Register/clear size-matched CJK UI fallbacks (see fallbackFontMap_).
  // setFallbackFont maps a primary UI font id to an SD font id of the same size.
  void setFallbackFont(int primaryFontId, int fallbackFontId) { fallbackFontMap_[primaryFontId] = fallbackFontId; }
  void clearFallbackFonts() { fallbackFontMap_.clear(); }
  // Ensure SD card font glyph data is loaded for the given text. Called from layout code
  // (which holds a const GfxRenderer&) before measuring word widths. Safe to call on non-SD fonts (returns 0).
  // styleMask: bitmask of styles to prepare (bit 0=regular, 1=bold, 2=italic, 3=bold-italic).
  // Returns the number of glyphs that will not render from a font: for Cached SD fonts with a glyph
  // fallback, the uncovered emoji-cluster bases the fallback font lacks too (0 when all render).
  int ensureSdCardFontReady(int fontId, const char* utf8Text, uint8_t styleMask = 0x0F) const;
  // `wordStyles` (optional, parallel to words) limits each word to its own style's advance table.
  // Also prepares the fallback font's advances for glyphs the font lacks (flash fonts included).
  int ensureSdCardFontReady(int fontId, const std::vector<std::string>& words, bool includeHyphen,
                            uint8_t styleMask = 0x0F,
                            const std::vector<EpdFontFamily::Style>* wordStyles = nullptr) const;
  // Replaces the SD font's resident glyph set with this text's glyphs. No-op (returns 0) while a
  // FontCacheManager page prewarm is pinned; the text's glyphs then load on demand.
  int prewarmSdCardFont(int fontId, const char* utf8Text, uint8_t styleMask = 0x0F) const;

  // Glyph fallback. When the font lacks a visible codepoint, drawing and
  // measuring borrow it from the provider's fallback font (e.g. a symbol and
  // emoji .cpfont); if that lacks it too, one small dotted mark is drawn per
  // grapheme cluster. Never used for BoundedUI SD fonts.
  void setFallbackFontProvider(FallbackFontProviderFn provider, void* context) {
    fallbackProvider_ = provider;
    fallbackContext_ = context;
  }
  // Identity of the installed fallback font (0 when none). Layout caches mix it
  // into their font key, since fallback glyphs change measured widths.
  void setGlyphLayoutKey(uint32_t key) { glyphLayoutKey_ = key; }
  uint32_t glyphLayoutKey() const { return glyphLayoutKey_; }
  SdCardFont* fallbackFont(bool load) const {
    return fallbackProvider_ ? fallbackProvider_(fallbackContext_, load) : nullptr;
  }
  // Appends (UTF-8, unique) the visible cluster-base codepoints of `text` that
  // fontId lacks. Used to prewarm the fallback font with a page's misses.
  void appendFallbackCodepoints(int fontId, const char* text, std::string& out) const;
  // Replaces the fallback font's resident glyph set; no-op for empty text or no fallback.
  int prewarmFallbackFont(const char* utf8Text) const;
  void clearFallbackCache() const;
  // Advance of the mark drawn for a cluster no installed font covers (12.4 fixed point).
  static int32_t missingGlyphAdvanceFP(const EpdFontData* data);
  // Packed variant for the paragraph layout path: each segment holds
  // consecutive NUL-terminated words (WordStore chunks), so a whole paragraph
  // is scanned without materializing per-word strings.
  void ensureSdCardFontReady(int fontId, const char* const* segments, const size_t* segmentLens, size_t segmentCount,
                             bool includeSpace, bool includeHyphen, uint8_t styleMask = 0x0F) const;

  // Orientation control (affects logical width/height and coordinate transforms)
  void setOrientation(const Orientation o) { orientation = o; }
  Orientation getOrientation() const { return orientation; }

  // Fading fix control
  void setFadingFix(const bool enabled) { fadingFix = enabled; }

  // Screen ops
  int getScreenWidth() const;
  int getScreenHeight() const;
  void tapToLogical(float nx, float ny, int& outX, int& outY) const;
  void displayBuffer(HalDisplay::RefreshMode refreshMode = HalDisplay::FAST_REFRESH) const;
  // One-shot: the next displayBuffer()/displayBufferAsync() call uses `mode`
  // instead of what its caller asked for, then the override clears itself.
  // Lets a closing overlay (the control center's refresh tile) hand a
  // ghost-cleanup waveform to the repaint of whatever screen is underneath,
  // which it cannot reach directly.
  void promoteNextRefresh(const HalDisplay::RefreshMode mode) const {
    promotedRefreshPending_ = true;
    promotedRefresh_ = mode;
  }
  // Non-blocking refresh: starts the waveform and returns so CPU work (e.g.
  // grayscale strip rendering) can overlap the panel's refresh time. The
  // framebuffer must stay untouched until waitRefreshComplete(). Falls back to
  // a blocking refresh when fadingFix is enabled or the panel lacks deferral
  // support. See HalDisplay::displayBufferAsync for the baseline contract.
  void displayBufferAsync(HalDisplay::RefreshMode refreshMode = HalDisplay::FAST_REFRESH) const;
  void waitRefreshComplete() const;
  // True when displayBufferAsync() genuinely overlaps: panel defers and
  // fadingFix isn't forcing the blocking path. Callers can skip overlap
  // scaffolding (e.g. whole-plane grayscale buffers) when false.
  bool supportsAsyncRefresh() const;
  // True when the display can overlap an ordinary B/W refresh with grayscale
  // composition without bypassing a required grayscale base waveform.
  HalDisplay::GrayscaleCapabilities grayscaleCapabilities(
      HalDisplay::GrayscaleMode mode = HalDisplay::GrayscaleMode::Overlay) const;
  // Compatibility queries for Overlay mode.
  bool supportsAsyncGrayscaleBase() const;
  // EXPERIMENTAL: Windowed update - display only a rectangular region
  // void displayWindow(int x, int y, int width, int height) const;
  void invertScreen() const;
  void clearScreen(uint8_t color = 0xFF) const;
  void getOrientedViewableTRBL(int* outTop, int* outRight, int* outBottom, int* outLeft) const;

  // Tiled grayscale strip target. While active, drawPixel() and clearScreen()
  // operate on `scratch` (panelWidthBytes * stripRows bytes, holding physical
  // rows [stripY0, stripY0 + stripRows)) instead of the framebuffer; pixels
  // whose physical row falls outside the band are clipped. The clip is applied
  // after the orientation rotate, so it is orientation-agnostic. Used to render
  // grayscale planes band-by-band without a full second buffer.
  void beginStripTarget(uint8_t* scratch, int stripY0, int stripRows) const;
  void endStripTarget() const;

  // Band culling for tiled grayscale. Takes a glyph bounding box in logical
  // screen coords and returns false only when a strip is active AND the box's
  // physical y-extent lies entirely outside the active band, letting callers
  // skip an expensive bitmap decode. Returns true when no strip is active.
  // Corners are rotated to physical, so it is orientation-aware.
  bool glyphIntersectsStrip(int x0, int y0, int x1, int y1) const;

  // Ink row tracking for tiled-grayscale culling. Between beginInkRows() and
  // endInkRows(), the box of every glyph, missing-glyph mark and line drawn
  // outside strip mode widens a physical row range (a superset of the rows
  // their pixels touch); endInkRows() returns it (false when nothing was
  // drawn). Raw writers (DirectPixelWriter, fillRect, single drawPixel calls)
  // are not tracked, so only use it around text drawing.
  void beginInkRows() const {
    _inkTracking = true;
    _inkMinRow = panelHeight;
    _inkMaxRow = -1;
  }
  // Widens the tracked rows by a logical box (no-op unless tracking outside strips).
  void noteInk(int x0, int y0, int x1, int y1) const;
  bool endInkRows(int* minRow, int* maxRow) const {
    _inkTracking = false;
    *minRow = _inkMinRow;
    *maxRow = _inkMaxRow;
    return _inkMaxRow >= _inkMinRow;
  }

  // Active pixel-write target for raw writers (DirectPixelWriter) that bypass
  // drawPixel for speed. When a strip target is active these return the band
  // scratch plus its physical-row origin and extent; otherwise the full
  // framebuffer ([0, panelHeight)). Writers subtract the origin and clip to the
  // extent, so they honor tiled-grayscale banding without per-pixel method calls.
  uint8_t* getWriteTarget() const { return _stripActive ? _stripBuf : frameBuffer; }
  int getWriteOriginY() const { return _stripActive ? _stripY0 : 0; }
  int getWriteRows() const { return _stripActive ? _stripRows : panelHeight; }

  // Drawing
  // UI drawing clip in logical coordinates; independent of panel orientation.
  std::array<int, 4> getClipRect() const {
    return {clipLeft_, clipTop_, clipRight_ - clipLeft_, clipBottom_ - clipTop_};
  }
  void setClipRect(int x, int y, int width, int height) const {
    clipLeft_ = x;
    clipTop_ = y;
    clipRight_ = x + width;
    clipBottom_ = y + height;
  }
  void drawPixel(int x, int y, bool state = true) const;
  // One glyph bitmap (packed 1-bit, or 2-bit where `mode` picks the levels drawn) with its
  // top-left pixel at logical (x0, y0): the same pixels, clip and strip redirection as
  // drawPixel() per pixel, with the orientation transform hoisted out of the loop.
  void blitGlyph(const uint8_t* bitmap, bool is2Bit, int width, int height, int x0, int y0, RenderMode mode,
                 bool pixelState) const;
  // Draw glyph ink with clipping and orientation resolved once per glyph.
  void drawGlyphBitmap(const uint8_t* bitmap, int width, int height, const glyphBitmap::Frame& frame, bool twoBit,
                       RenderMode mode, bool state) const;
  void drawLine(int x1, int y1, int x2, int y2, bool state = true) const;
  void drawLine(int x1, int y1, int x2, int y2, int lineWidth, bool state) const;
  void drawArc(int maxRadius, int cx, int cy, int xDir, int yDir, int lineWidth, bool state) const;
  void drawRect(int x, int y, int width, int height, bool state = true) const;
  void drawRect(int x, int y, int width, int height, int lineWidth, bool state) const;
  void drawRoundedRect(int x, int y, int width, int height, int lineWidth, int cornerRadius, bool state) const;
  void drawRoundedRect(int x, int y, int width, int height, int lineWidth, int cornerRadius, bool roundTopLeft,
                       bool roundTopRight, bool roundBottomLeft, bool roundBottomRight, bool state) const;
  void maskRoundedRectOutsideCorners(int x, int y, int width, int height, int radius, Color color = Color::White) const;
  void fillRect(int x, int y, int width, int height, bool state = true) const;
  void fillRectDither(int x, int y, int width, int height, Color color) const;
  void fillRoundedRect(int x, int y, int width, int height, int cornerRadius, Color color) const;
  void fillRoundedRect(int x, int y, int width, int height, int cornerRadius, bool roundTopLeft, bool roundTopRight,
                       bool roundBottomLeft, bool roundBottomRight, Color color) const;
  void drawImage(const uint8_t bitmap[], int x, int y, int width, int height) const;
  void drawIcon(const uint8_t bitmap[], int x, int y, int width, int height) const;
  bool drawBitmap(const Bitmap& bitmap, int x, int y, int maxWidth, int maxHeight, float cropX = 0, float cropY = 0,
                  bool allowUpscale1Bit = false, bool whiteAsTransparent = false) const;
  bool drawBitmap1Bit(const Bitmap& bitmap, int x, int y, int maxWidth, int maxHeight, bool allowUpscale = false) const;
  void drawIcon(const uint8_t bitmap[], int x, int y, int size) const;
  // Counter-invert content images in the logical framebuffer so output-level
  // dark mode leaves their original polarity unchanged.
  void preserveImagePolarity(int x, int y, int width, int height) const;
  void fillPolygon(const int* xPoints, const int* yPoints, int numPoints, bool state = true) const;

  // Snapshot / restore a screen-coordinate framebuffer region (byte-aligned in
  // panel memory). readFramebufferRegion returns the bytes written to dst, or
  // 0 when the region is empty, offscreen, or exceeds dstCapacity. Pass the
  // same rectangle to writeFramebufferRegion to restore the saved pixels.
  // Enables partial-repaint patterns (e.g. moving a selection highlight)
  // without re-rendering the whole page.
  size_t readFramebufferRegion(int x, int y, int w, int h, uint8_t* dst, size_t dstCapacity) const;
  void writeFramebufferRegion(int x, int y, int w, int h, const uint8_t* src);

  // Text
  // Layout may use advance-only SD font tables; rendered measurement includes kerning and ligatures.
  enum class TextMeasureMode { Layout, Rendered };
  int getTextWidth(int fontId, const char* text, EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                   BidiUtils::BidiBaseDir baseDir = BidiUtils::BidiBaseDir::AUTO) const;
  void drawCenteredText(int fontId, int y, const char* text, bool black = true,
                        EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                        BidiUtils::BidiBaseDir baseDir = BidiUtils::BidiBaseDir::AUTO) const;
  void drawText(int fontId, int x, int y, const char* text, bool black = true,
                EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                BidiUtils::BidiBaseDir baseDir = BidiUtils::BidiBaseDir::AUTO, int8_t tracking = 0) const;
  int getSpaceWidth(int fontId, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  /// Returns the total inter-word advance: fp4::toPixel(spaceAdvance + kern(leftCp,' ') + kern(' ',rightCp)).
  /// Using a single snap avoids the +/-1 px rounding error that arises when space advance and kern are
  /// snapped separately and then added as integers.
  int getSpaceAdvance(int fontId, uint32_t leftCp, uint32_t rightCp, EpdFontFamily::Style style) const;
  /// Returns kerning plus optional tracking between two adjacent codepoints.
  int getKerning(int fontId, uint32_t leftCp, uint32_t rightCp, EpdFontFamily::Style style, int8_t tracking = 0) const;
  int getTextAdvanceX(int fontId, const char* text, EpdFontFamily::Style style, int8_t tracking = 0,
                      BidiUtils::BidiBaseDir baseDir = BidiUtils::BidiBaseDir::AUTO,
                      TextMeasureMode mode = TextMeasureMode::Layout) const;
  int getFontAscenderSize(int fontId) const;
  int getLineHeight(int fontId) const;
  int getLineHeight(int fontId, float compression) const;
  std::string truncatedText(int fontId, const char* text, int maxWidth,
                            EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  /// Word-wrap \p text into at most \p maxLines lines, each no wider than
  /// \p maxWidth pixels. Overflowing words and excess lines are UTF-8-safely
  /// truncated with an ellipsis (U+2026).
  std::vector<std::string> wrappedText(int fontId, const char* text, int maxWidth, int maxLines,
                                       EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;

  // Helper for drawing rotated text (90 degrees clockwise, for side buttons)
  void drawTextRotated90CW(int fontId, int x, int y, const char* text, bool black = true,
                           EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getTextHeight(int fontId) const;

  // Grayscale functions
  void setRenderMode(RenderMode mode);
  RenderMode getRenderMode() const { return renderMode; }
  // Grayscale preconditioning settle pass (no-op on X4). The rect overload
  // takes the gray region in LOGICAL screen coordinates and rotates it to the
  // panel; the no-arg overload settles the full frame. Call after the BW base
  // frame is displayed and before the grayscale planes are written.
  void preconditionGrayscale() const;
  void preconditionGrayscale(int x, int y, int w, int h) const;
  // Display the framebuffer as the base frame for a grayscale overlay that
  // follows (X3: OEM differential base waveform; others: plain display with
  // `fallback`).
  void displayGrayscaleBase(HalDisplay::RefreshMode fallback = HalDisplay::HALF_REFRESH) const;
  bool displayGrayscaleBase(HalDisplay::GrayscaleMode mode,
                            HalDisplay::RefreshMode fallback = HalDisplay::HALF_REFRESH) const;
  void copyGrayscaleLsbBuffers() const;
  void copyGrayscaleMsbBuffers() const;
  void displayGrayBuffer() const;
  // Active input encoding, used when drawing monochrome overlays into planes.
  bool grayPlanesAreAbsolute() const { return absoluteGrayPlanes; }

  // Tiled grayscale (X4): stream one band of a plane straight to controller RAM
  // from `scratch` (panelWidthBytes * numRows, physical rows [yStart, yStart+
  // numRows)), bypassing the framebuffer. supportsStripGrayscale() gates use.
  void writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t* scratch, int yStart, int numRows) const;
  bool supportsStripGrayscale() const;
  // Paper Mono: the base activation is deferred so base + gray planes go out
  // as one waveform. Route the base through displayGrayscaleBase() when true.
  bool combinesGrayscaleBase() const;
  bool storeBwBuffer();  // Returns true if buffer was stored successfully
  // Restore and free the stored buffer. resyncPanelBaseline rewrites the
  // controller's differential baseline to the restored frame — correct after
  // a grayscale render (the glass matches the stored BW plane), WRONG when
  // the glass shows content painted after the store (overlay chrome): the
  // next differential would treat that content as already erased and leave
  // it on the glass. Such callers pass false so the baseline keeps tracking
  // what was last pushed.
  void restoreBwBuffer(bool resyncPanelBaseline = true);
  // Free a stored buffer without restoring it (the page under it changed).
  void discardStoredBwBuffer() { freeBwBufferChunks(); }
  void cleanupGrayscaleWithFrameBuffer() const;

  // Font helpers
  const uint8_t* getGlyphBitmap(const EpdFontData* fontData, const EpdGlyph* glyph) const;

  // Lend the 48 KB framebuffer's bytes to a memory-hungry phase (chapter
  // builds) WITHOUT freeing the allocation, so it never moves and repeated
  // loans cannot fragment the heap. Between release and restore NOTHING may
  // draw or display — the panel keeps showing its last refreshed image. The
  // lent bytes are published via buildscratch::claim() for consumers like
  // InflateStream. restore returns the buffer white, so the caller must
  // redraw the full screen; it cannot fail (no allocation involved).
  void releaseFrameBufferForBuild();
  bool restoreFrameBufferAfterBuild();
  bool hasFrameBuffer() const { return frameBuffer != nullptr; }
  // One-way ownership transfer before sleep. Call under RenderLock, after
  // saving the retained frame; drawing stays disabled until the next boot.
  bool releaseFrameBufferForSleep();

  // RAII form of the loan above, for blocking build regions with early-return
  // error paths: restores on scope exit (or explicitly via end()). Display the
  // popup/screen the panel should hold BEFORE constructing one. Constructing
  // while the framebuffer is already lent yields an inert loan (nesting-safe).
  class FrameBufferLoan {
   public:
    explicit FrameBufferLoan(GfxRenderer& renderer);
    ~FrameBufferLoan() { end(); }
    void end();
    FrameBufferLoan(const FrameBufferLoan&) = delete;
    FrameBufferLoan& operator=(const FrameBufferLoan&) = delete;

   private:
    GfxRenderer& renderer_;
    bool active_ = false;
  };

  // Low level functions
  uint8_t* getFrameBuffer() const;
  size_t getBufferSize() const;
  uint16_t getDisplayWidth() const { return panelWidth; }
  uint16_t getDisplayHeight() const { return panelHeight; }
  uint16_t getDisplayWidthBytes() const { return panelWidthBytes; }

  // Region cache: take a logical (orientation-aware) rect, hit the framebuffer
  // bytes that the rect can have touched, and pump them in or out of a caller-
  // supplied buffer. Used by HomeActivity to snapshot just the cover tile
  // (~16 KB in Portrait) instead of cloning the entire 48 KB framebuffer.
  //
  // getRegionByteSize: required buffer length for the rect at current orientation.
  // copyRegionToBuffer / copyBufferToRegion: false if `bufSize` is smaller than that.
  size_t getRegionByteSize(int logicalX, int logicalY, int logicalW, int logicalH) const;
  bool copyRegionToBuffer(int logicalX, int logicalY, int logicalW, int logicalH, uint8_t* buf, size_t bufSize) const;
  bool copyBufferToRegion(int logicalX, int logicalY, int logicalW, int logicalH, const uint8_t* buf,
                          size_t bufSize) const;
};
