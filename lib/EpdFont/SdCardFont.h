#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <string>
#include <vector>

#include "EpdFont.h"
#include "EpdFontData.h"
#include "EpdFontFamily.h"

// On-disk binary format version for .cpfont files. Defined as a preprocessor
// macro (rather than a constexpr) so it can be stringified into the SD-fonts
// release URL — see FONT_MANIFEST_URL in FontDownloadActivity.h. No integer
// suffix because stringification would include it (e.g. `4U` → `"4U"`).
//
// The canonical version for the build tooling lives in
// lib/EpdFont/scripts/cpfont_version.py. This firmware-side copy must be
// bumped manually when the firmware is updated to support a new format.
// Reader enforcement: SdCardFont::load().
#define CPFONT_VERSION 4

class SdCardFont {
 public:
  static constexpr uint16_t MAX_PAGE_GLYPHS = 512;
  static constexpr uint8_t MAX_STYLES = 4;
  enum class LoadMode : uint8_t { Cached, BoundedUI };
  // BoundedUI is for small UI fonts, not general book typography. Interval
  // tables stay on SD; the existing eight-slot glyph cache is byte-capped.
  static constexpr uint16_t UI_GLYPH_BYTES = 256;

  SdCardFont() = default;
  ~SdCardFont();
  // Owns raw buffers freed in dtor — no shallow-copy semantics. Make any
  // accidental pass-by-value or move a compile-time error.
  SdCardFont(const SdCardFont&) = delete;
  SdCardFont& operator=(const SdCardFont&) = delete;
  SdCardFont(SdCardFont&&) = delete;
  SdCardFont& operator=(SdCardFont&&) = delete;

  // Load .cpfont file: reads header + intervals into RAM, records file layout offsets.
  // Supports v4 (multi-style) format.
  // Returns true on success.
  bool load(const char* path, LoadMode mode = LoadMode::Cached, void (*progress)() = nullptr);
  LoadMode loadMode() const { return loadMode_; }
  // Sticky per load: a presenter must not certify a frame after a disk-backed
  // shaping/read/allocation failure was replaced by fallback output.
  bool boundedReadFailed() const { return boundedReadFailed_; }

  // Pre-read glyphs needed for the given UTF-8 text from SD card.
  // styleMask: bitmask of styles to prewarm (bit 0=regular, 1=bold, 2=italic, 3=bolditalic).
  // Default 0x0F = all present styles.
  // When metadataOnly=true, only glyph metrics are loaded (no bitmap data).
  // Returns number of glyphs that couldn't be loaded (0 on full success).
  // extraText (optional): up to MAX_EXTRA_GLYPHS further codepoints (UI text such as the
  // status-bar title) whose glyphs load with the page's but stay out of the per-page
  // kerning classes, so they render exactly as the on-demand overflow path draws them.
  int prewarm(const char* utf8Text, uint8_t styleMask = 0x0F, bool metadataOnly = false,
              const char* extraText = nullptr);
  static constexpr uint8_t MAX_EXTRA_GLYPHS = 32;

  // Prepare advance widths for layout measurement. Every glyph the request
  // needs is resident when the call returns (bounded LRU, see
  // ADVANCE_CACHE_LIMIT): misses are batched and read with one file open per
  // ADVANCE_FETCH_BATCH codepoints, sorted by glyph index. Invisible
  // (default-ignorable) codepoints are skipped: renderers give them no width.
  // Returns number of codepoints (per style) not covered by the font.
  int buildAdvanceTable(const char* utf8Text, uint8_t styleMask = 0x0F);
  // `wordStyles`, when given, is parallel to `words`: each word's codepoints
  // are prepared only for its own (resolved) style instead of every style in
  // styleMask. Space and, when includeHyphen, '-' are prepared for each style.
  int buildAdvanceTable(const std::vector<std::string>& words, bool includeHyphen, uint8_t styleMask = 0x0F,
                        const std::vector<EpdFontFamily::Style>* wordStyles = nullptr);
  int buildAdvanceTable(const uint32_t* codepoints, uint32_t count, uint8_t styleMask);

  // Look up advanceX for a codepoint from the advance table.
  // Returns the 12.4 fixed-point advance, or 0 if not found.
  uint16_t getAdvance(uint32_t codepoint, uint8_t style) const;
  // Same lookup, distinguishing "not prepared" from a zero advance.
  bool lookupAdvance(uint32_t codepoint, uint8_t style, uint16_t* outAdvance) const;

  // Returns true if advance table is populated for at least one style.
  bool hasAdvanceTable() const;

  // True when the font file maps `codepoint` for the (resolved) style. Cached
  // mode answers from the resident interval table without SD I/O; BoundedUI
  // mode reads intervals from SD (with a small negative cache).
  bool hasGlyph(uint32_t codepoint, uint8_t style) const;

  // Cross-page glyph reuse. Moves the current page's resident glyph set aside (instead of
  // freeing it) so the next prewarm copies the glyphs both pages share rather than reading
  // them from SD again (a Japanese page shares ~45% of its glyphs with the previous one, a
  // Latin page ~75%). The set is not used for drawing; the next prewarm consumes and frees
  // it, dropRetainedGlyphs() frees it early. Returns the bytes now retained.
  uint32_t retainPageGlyphs();
  void dropRetainedGlyphs();
  uint32_t retainedGlyphBytes() const;
  // Bytes the current resident glyph set holds (bitmaps, records, intervals).
  uint32_t residentGlyphBytes() const;

  // Free mini data for all styles and restore stub EpdFontData.
  // Preserves the persistent advance cache (trimmed back to
  // ADVANCE_CACHE_LIMIT per style) so repeated layout passes can reuse
  // previously fetched metrics.
  void clearCache();

  // Drop the persistent advance cache. Call when unloading the SD font or
  // when font/size/family/glyph-table state changes.
  void clearPersistentCache();

  // Returns pointer to the managed EpdFont for a given style.
  // Returns nullptr if the style is not present.
  EpdFont* getEpdFont(uint8_t style = 0);

  // Returns true if the given style is present in this font file.
  bool hasStyle(uint8_t style) const;

  // Resolve requested style bits to the closest present style.
  uint8_t resolveStyle(uint8_t style) const;

  // Resolve every requested style bit through fallback and return the actual
  // styles that need cache/advance preparation.
  uint8_t resolveStyleMask(uint8_t styleMask) const;

  // Number of styles present in this font file.
  uint8_t styleCount() const { return styleCount_; }

  // Returns true if the glyph pointer points into the overflow buffer.
  bool isOverflowGlyph(const EpdGlyph* glyph) const;

  // Returns the bitmap for an on-demand-loaded (overflow) glyph.
  const uint8_t* getOverflowBitmap(const EpdGlyph* glyph) const;

  // Extract SdCardFont* from an opaque glyphMissCtx pointer.
  // Used by GfxRenderer::getGlyphBitmap() to recover the SdCardFont from EpdFontData::glyphMissCtx.
  static SdCardFont* fromMissCtx(void* ctx);

  struct Stats {
    uint32_t prewarmTotalMs = 0;
    uint32_t sdReadTimeMs = 0;
    uint32_t seekCount = 0;
    uint32_t uniqueGlyphs = 0;
    uint32_t bitmapBytes = 0;
    uint32_t advanceFileOpens = 0;  // advance-table batch reads (one open each)
    uint32_t advanceEvictions = 0;  // advance entries evicted to stay bounded
    uint32_t uniformScans = 0;      // intervals read whole to detect uniform advances
    uint32_t reusedGlyphs = 0;      // glyphs copied from the retained previous page set
  };
  void logStats(const char* label = "SDCF");
  void resetStats();
  const Stats& getStats() const { return stats_; }

  // Content hash of the file header + style TOC entries (computed during load).
  // Used to generate deterministic font IDs for section cache invalidation.
  uint32_t contentHash() const { return contentHash_; }

 private:
  // Per-style metadata (parsed from file header/TOC)
  struct CpFontHeader {
    uint32_t intervalCount = 0;
    uint32_t glyphCount = 0;
    uint8_t advanceY = 0;
    int16_t ascender = 0;
    int16_t descender = 0;
    bool is2Bit = false;
    uint16_t kernLeftEntryCount = 0;
    uint16_t kernRightEntryCount = 0;
    uint8_t kernLeftClassCount = 0;
    uint8_t kernRightClassCount = 0;
    uint8_t ligaturePairCount = 0;
  };

  // All per-style data: file offsets, intervals, kern/lig, prewarm cache, EpdFont
  struct PerStyle {
    CpFontHeader header{};

    // File layout offsets for this style's data sections
    uint32_t intervalsFileOffset = 0;
    uint32_t glyphsFileOffset = 0;
    uint32_t kernLeftFileOffset = 0;
    uint32_t kernRightFileOffset = 0;
    uint32_t kernMatrixFileOffset = 0;
    uint32_t ligatureFileOffset = 0;
    uint32_t bitmapFileOffset = 0;

    // Full intervals loaded from file (kept in RAM for codepoint lookup)
    EpdUnicodeInterval* fullIntervals = nullptr;
    struct BmpInterval16 {
      uint16_t first;
      uint16_t last;
      uint16_t offset;
    } __attribute__((packed));
    static_assert(sizeof(BmpInterval16) == 6, "BmpInterval16 must remain compact");
    BmpInterval16* bmpIntervals = nullptr;
    bool intervalsAreBmp16 = false;

    // Persistent kern-class + ligature tables (lazy-loaded on first prewarm).
    // The full kern MATRIX is NOT resident — on Literata-class fonts a single
    // style's matrix is ~36-42KB contiguous, and 4 styles' worth won't fit
    // alongside bitmaps + framebuffer on a 380KB device. Only kernLeftClasses
    // and kernRightClasses (small codepoint→classId tables, ~3KB each) stay
    // resident; the matrix is reconstructed per-page as miniKernMatrix.
    EpdKernClassEntry* kernLeftClasses = nullptr;
    EpdKernClassEntry* kernRightClasses = nullptr;
    EpdLigaturePair* ligaturePairs = nullptr;
    bool kernLigLoaded = false;

    // Stub EpdFontData returned when not prewarmed
    EpdFontData stubData{};

    // Mini EpdFontData built during prewarm
    EpdFontData miniData{};
    EpdUnicodeInterval* miniIntervals = nullptr;
    EpdGlyph* miniGlyphs = nullptr;
    uint8_t* miniBitmap = nullptr;
    uint32_t miniIntervalCount = 0;
    uint32_t miniGlyphCount = 0;

    // Per-page mini kern matrix (built by buildMiniKernMatrix on each full
    // prewarm). miniKernLeftClasses/miniKernRightClasses map ONLY the codepoints
    // used on the current page to renumbered class IDs (1..miniKern*ClassCount).
    // miniKernMatrix is a small miniKernLeftClassCount × miniKernRightClassCount
    // flat matrix. Typical Latin page: ~25×25 matrix = ~625 bytes per style vs
    // ~36KB for the full Literata matrix — ~50× reduction.
    EpdKernClassEntry* miniKernLeftClasses = nullptr;
    EpdKernClassEntry* miniKernRightClasses = nullptr;
    uint16_t miniKernLeftEntryCount = 0;
    uint16_t miniKernRightEntryCount = 0;
    uint8_t miniKernLeftClassCount = 0;
    uint8_t miniKernRightClassCount = 0;
    int8_t* miniKernMatrix = nullptr;

    // Previous page's glyph set, kept for reuse by the next prewarm (retainPageGlyphs).
    EpdUnicodeInterval* prevIntervals = nullptr;
    EpdGlyph* prevGlyphs = nullptr;
    uint8_t* prevBitmap = nullptr;
    uint32_t prevIntervalCount = 0;
    uint32_t prevGlyphCount = 0;
    uint32_t prevBitmapBytes = 0;
    uint32_t miniBitmapBytes = 0;

    // The EpdFont whose data pointer we manage
    EpdFont epdFont{&stubData};

    bool present = false;
  };

  PerStyle styles_[MAX_STYLES] = {};
  uint8_t styleCount_ = 0;

  char filePath_[128] = {};

  // Overflow context: glyphMissHandler needs to know which style it's serving
  struct OverflowContext {
    SdCardFont* self;
    uint8_t styleIdx;
  };
  OverflowContext overflowCtx_[MAX_STYLES] = {};

  // Shared on-demand overflow buffer (ring buffer of glyphs loaded via glyphMissHandler)
  static constexpr uint32_t OVERFLOW_CAPACITY = 8;
  struct OverflowEntry {
    EpdGlyph glyph;
    uint8_t* bitmap = nullptr;
    uint32_t codepoint = 0;
    uint8_t styleIdx = 0;
  };
  OverflowEntry overflow_[OVERFLOW_CAPACITY] = {};
  uint32_t overflowCount_ = 0;
  uint32_t overflowNext_ = 0;

  // Advance-only metrics for layout measurement, one table per style, sorted
  // by codepoint for binary lookup. It is a bounded cache, not a coverage
  // map: each request stamps the entries it uses with a generation number,
  // and when room is needed the least recently requested entries are evicted.
  // The steady-state bound is ADVANCE_CACHE_LIMIT entries per style; a single
  // request needing more (one 750-word flush of dense CJK text) may grow the
  // table to ADVANCE_REQUEST_LIMIT until the next clearCache() trims it back.
  // Entries persist across layout passes; clearPersistentCache() drops them.
  struct AdvanceEntry {
    uint32_t codepoint;
    uint16_t advanceX;  // 12.4 fixed-point
    uint16_t lastUse;   // request generation; occupies what used to be padding
  };
  static_assert(sizeof(AdvanceEntry) == 8, "AdvanceEntry must stay 8 bytes");
  struct AdvanceTable {
    AdvanceEntry* entries = nullptr;
    uint32_t size = 0;
    uint32_t capacity = 0;
  };

 public:
  static constexpr uint32_t ADVANCE_CACHE_LIMIT = 768;     // 6 KiB per style
  static constexpr uint32_t ADVANCE_REQUEST_LIMIT = 2048;  // 16 KiB per style, transient
  static constexpr uint32_t ADVANCE_FETCH_BATCH = 256;     // misses per SD open
  static constexpr uint32_t ADVANCE_INITIAL_CAPACITY = 128;
  uint32_t advanceCacheSize(uint8_t style) const { return advance_[style & (MAX_STYLES - 1)].size; }
  uint32_t advanceCacheCapacity(uint8_t style) const { return advance_[style & (MAX_STYLES - 1)].capacity; }

 private:
  AdvanceTable advance_[MAX_STYLES] = {};
  uint16_t advanceGeneration_ = 0;

  // Uniform-advance runs. Large intervals such as Hangul syllables (11,172
  // glyphs) or CJK ideographs usually share one advance. When one batch misses
  // many glyphs of such an interval, the interval's glyph records are read once,
  // sequentially, and kept as codepoint ranges if they collapse into at most
  // UNIFORM_MAX_RUNS_PER_INTERVAL runs; otherwise the interval is remembered as
  // tried and the LRU table serves it. Allocated per style on first scan.
  struct AdvanceRun {
    uint32_t first;
    uint32_t last;
    uint16_t advanceX;  // 12.4 fixed-point
  };
  struct UniformAdvances {
    static constexpr uint8_t MAX_RUNS = 16;
    static constexpr uint8_t MAX_TRIED = 8;
    AdvanceRun runs[MAX_RUNS] = {};
    uint32_t tried[MAX_TRIED] = {};  // interval indices already scanned (kept or rejected)
    uint8_t runCount = 0;
    uint8_t triedCount = 0;
  };
  static constexpr uint32_t UNIFORM_SCAN_MIN_SPAN = 1024;
  static constexpr uint32_t UNIFORM_SCAN_MIN_MISSES = 48;
  static constexpr uint8_t UNIFORM_MAX_RUNS_PER_INTERVAL = 4;
  UniformAdvances* uniform_[MAX_STYLES] = {};
  const AdvanceRun* findUniformAdvance(uint8_t styleIdx, uint32_t codepoint) const;
  // Cached mode only: index of the interval containing codepoint, or -1.
  static int32_t findIntervalIndex(const PerStyle& s, uint32_t codepoint);
  static bool intervalAt(const PerStyle& s, uint32_t index, EpdUnicodeInterval* out);
  struct AdvanceRequest;
  friend struct AdvanceRequest;
  uint16_t beginAdvanceRequest();
  AdvanceEntry* findAdvanceEntry(uint8_t styleIdx, uint32_t codepoint) const;
  bool growAdvanceTable(uint8_t styleIdx, uint32_t newCapacity);
  uint32_t evictStaleAdvances(uint8_t styleIdx, uint32_t count, uint16_t protectedGeneration);
  uint32_t reserveAdvanceSlots(uint8_t styleIdx, uint32_t needed, uint16_t generation);
  void insertAdvances(uint8_t styleIdx, const AdvanceEntry* sortedNew, uint32_t count);
  void trimAdvanceCache();

  // BoundedUI glyph misses read the interval table from SD. Remember the most
  // recent misses so a page full of an unsupported symbol does not reopen the
  // font for every occurrence and every render pass.
  static constexpr uint8_t MISS_CACHE_SIZE = 8;
  mutable uint32_t missCache_[MISS_CACHE_SIZE] = {};
  mutable uint8_t missCacheCount_ = 0;
  mutable uint8_t missCacheNext_ = 0;
  bool recentlyMissed(uint32_t codepoint, uint8_t styleIdx) const;
  void rememberMiss(uint32_t codepoint, uint8_t styleIdx) const;

  Stats stats_;
  uint32_t contentHash_ = 0;
  bool loaded_ = false;
  LoadMode loadMode_ = LoadMode::Cached;
  void (*progress_)() = nullptr;
  uint32_t loadedFileSize_ = 0;
  mutable bool boundedReadFailed_ = false;
  int checkBoundedText(const char* text, uint8_t styleMask);
  static int8_t lookupBoundedKerning(void* context, uint32_t left, uint32_t right);
  static uint32_t lookupBoundedLigature(void* context, uint32_t left, uint32_t right);

  // Per-style helpers
  void freeStyleMiniData(PerStyle& s);
  static void freeStylePrev(PerStyle& s);
  // Index into s.prevGlyphs of codepoint, or -1.
  static int32_t findPrevGlyph(const PerStyle& s, uint32_t codepoint);
  bool prewarmOutOfMemory_ = false;  // set by prewarmStyle on an allocation failure
  void freeStyleAll(PerStyle& s);
  void freeStyleKernLigatureData(PerStyle& s);
  void freeStyleMiniKern(PerStyle& s);
  bool loadStyleKernLigatureData(PerStyle& s);
  // `excluded` (sorted, may be null) lists codepoints kept out of the kerning classes.
  // `file` is the font file when the caller has it open (otherwise opened here).
  bool buildMiniKernMatrix(PerStyle& s, const uint32_t* codepoints, uint32_t cpCount, const uint32_t* excluded,
                           uint32_t excludedCount, HalFile& file);
  void applyKernLigaturePointers(PerStyle& s, EpdFontData& data) const;
  void applyGlyphMissCallback(uint8_t styleIdx);
  int32_t findGlobalGlyphIndex(const PerStyle& s, uint32_t codepoint) const;
  int prewarmStyle(uint8_t styleIdx, const uint32_t* codepoints, uint32_t cpCount, bool metadataOnly,
                   const uint32_t* kernExcluded, uint32_t kernExcludedCount);

  // Global helpers
  void freeAll();
  void clearOverflow();
  static void computeStyleFileOffsets(PerStyle& s, uint32_t baseOffset);

  // Static callback for EpdFontData::glyphMissHandler (per-style via OverflowContext)
  static const EpdGlyph* onGlyphMiss(void* ctx, uint32_t codepoint);
};
