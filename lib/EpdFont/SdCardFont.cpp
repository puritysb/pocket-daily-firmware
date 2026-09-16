#include "SdCardFont.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstring>
#include <memory>

#include "EpdFontFamily.h"

static_assert(sizeof(EpdGlyph) == 16, "EpdGlyph must be 16 bytes to match .cpfont file layout");
static_assert(sizeof(EpdUnicodeInterval) == 12, "EpdUnicodeInterval must be 12 bytes to match .cpfont file layout");
static_assert(sizeof(EpdKernClassEntry) == 3, "EpdKernClassEntry must be 3 bytes to match .cpfont file layout");
static_assert(sizeof(EpdLigaturePair) == 8, "EpdLigaturePair must be 8 bytes to match .cpfont file layout");

namespace {

// FNV-1a hash for content-based font ID generation
constexpr uint32_t FNV_OFFSET = 2166136261u;
constexpr uint32_t FNV_PRIME = 16777619u;

uint32_t fnv1a(const uint8_t* data, size_t len, uint32_t hash = FNV_OFFSET) {
  for (size_t i = 0; i < len; i++) {
    hash ^= data[i];
    hash *= FNV_PRIME;
  }
  return hash;
}

// .cpfont magic bytes
constexpr char CPFONT_MAGIC[8] = {'C', 'P', 'F', 'O', 'N', 'T', '\0', '\0'};
// CPFONT_VERSION is defined as a #define in SdCardFont.h so it can be
// stringified into FONT_MANIFEST_URL.
constexpr uint32_t HEADER_SIZE = 32;
constexpr uint32_t STYLE_TOC_ENTRY_SIZE = 32;

// Helper to read little-endian values from byte buffer
inline uint16_t readU16(const uint8_t* p) { return p[0] | (p[1] << 8); }
inline int16_t readI16(const uint8_t* p) { return static_cast<int16_t>(p[0] | (p[1] << 8)); }
inline uint32_t readU32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); }

}  // namespace

SdCardFont::~SdCardFont() { freeAll(); }

// --- Per-style free/cleanup ---

void SdCardFont::freeStyleMiniData(PerStyle& s) {
  delete[] s.miniIntervals;
  s.miniIntervals = nullptr;
  delete[] s.miniGlyphs;
  s.miniGlyphs = nullptr;
  delete[] s.miniBitmap;
  s.miniBitmap = nullptr;
  s.miniIntervalCount = 0;
  s.miniGlyphCount = 0;
  freeStyleMiniKern(s);
  memset(&s.miniData, 0, sizeof(s.miniData));
  s.epdFont.data = &s.stubData;
}

void SdCardFont::freeStyleKernLigatureData(PerStyle& s) {
  // Both font views borrow the resident ligature table.
  s.stubData.ligaturePairs = nullptr;
  s.stubData.ligaturePairCount = 0;
  s.miniData.ligaturePairs = nullptr;
  s.miniData.ligaturePairCount = 0;
  delete[] s.kernLeftClasses;
  s.kernLeftClasses = nullptr;
  delete[] s.kernRightClasses;
  s.kernRightClasses = nullptr;
  delete[] s.ligaturePairs;
  s.ligaturePairs = nullptr;
  s.kernLigLoaded = false;
}

void SdCardFont::freeStyleMiniKern(PerStyle& s) {
  delete[] s.miniKernLeftClasses;
  s.miniKernLeftClasses = nullptr;
  delete[] s.miniKernRightClasses;
  s.miniKernRightClasses = nullptr;
  delete[] s.miniKernMatrix;
  s.miniKernMatrix = nullptr;
  s.miniKernLeftEntryCount = 0;
  s.miniKernRightEntryCount = 0;
  s.miniKernLeftClassCount = 0;
  s.miniKernRightClassCount = 0;
}

void SdCardFont::freeStyleAll(PerStyle& s) {
  freeStyleMiniData(s);
  delete[] s.fullIntervals;
  s.fullIntervals = nullptr;
  delete[] s.bmpIntervals;
  s.bmpIntervals = nullptr;
  s.intervalsAreBmp16 = false;
  freeStyleKernLigatureData(s);
  s.present = false;
}

// --- Global free/cleanup ---

void SdCardFont::freeAll() {
  clearOverflow();
  clearPersistentCache();
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    freeStyleAll(styles_[i]);
  }
  styleCount_ = 0;
  contentHash_ = 0;
  loaded_ = false;
  missCacheCount_ = 0;
  missCacheNext_ = 0;
}

void SdCardFont::clearOverflow() {
  for (uint32_t i = 0; i < overflowCount_; i++) {
    delete[] overflow_[i].bitmap;
    overflow_[i].bitmap = nullptr;
    overflow_[i].codepoint = 0;
  }
  overflowCount_ = 0;
  overflowNext_ = 0;
}

// --- Per-style kern/ligature ---

void SdCardFont::applyKernLigaturePointers(PerStyle& s, EpdFontData& data) const {
  // Kern data uses the per-page mini tables (renumbered class IDs). The full
  // kern matrix is never resident — see PerStyle::miniKernMatrix comment.
  data.kernLeftClasses = s.miniKernLeftClasses;
  data.kernRightClasses = s.miniKernRightClasses;
  data.kernMatrix = s.miniKernMatrix;
  data.kernLeftEntryCount = s.miniKernLeftEntryCount;
  data.kernRightEntryCount = s.miniKernRightEntryCount;
  data.kernLeftClassCount = s.miniKernLeftClassCount;
  data.kernRightClassCount = s.miniKernRightClassCount;
  // Ligatures are small (typically < 1KB) so they stay resident.
  data.ligaturePairs = s.ligaturePairs;
  data.ligaturePairCount = s.header.ligaturePairCount;
}

bool SdCardFont::loadStyleKernLigatureData(PerStyle& s) {
  if (s.kernLigLoaded) return true;
  bool hasKern = s.header.kernLeftEntryCount > 0;
  bool hasLig = s.header.ligaturePairCount > 0;
  if (!hasKern && !hasLig) {
    s.kernLigLoaded = true;
    return true;
  }

  HalFile file;
  if (!Storage.openFileForRead("SDCF", filePath_, file)) {
    LOG_ERR("SDCF", "Failed to open .cpfont for kern/lig: %s", filePath_);
    return false;
  }

  if (hasKern) {
    // Load only the small class-lookup tables (~3KB each). The full matrix
    // (~36KB contiguous for Literata) is built per-page from SD in
    // buildMiniKernMatrix().
    s.kernLeftClasses = new (std::nothrow) EpdKernClassEntry[s.header.kernLeftEntryCount];
    s.kernRightClasses = new (std::nothrow) EpdKernClassEntry[s.header.kernRightEntryCount];

    if (!s.kernLeftClasses || !s.kernRightClasses) {
      LOG_ERR("SDCF", "Failed to allocate kern classes (%u+%u bytes)", s.header.kernLeftEntryCount * 3u,
              s.header.kernRightEntryCount * 3u);
      freeStyleKernLigatureData(s);
      return false;
    }

    if (!file.seekSet(s.kernLeftFileOffset)) {
      LOG_ERR("SDCF", "Failed to seek to kern data");
      freeStyleKernLigatureData(s);
      return false;
    }
    size_t leftSz = s.header.kernLeftEntryCount * sizeof(EpdKernClassEntry);
    size_t rightSz = s.header.kernRightEntryCount * sizeof(EpdKernClassEntry);
    if (file.read(reinterpret_cast<uint8_t*>(s.kernLeftClasses), leftSz) != static_cast<int>(leftSz) ||
        file.read(reinterpret_cast<uint8_t*>(s.kernRightClasses), rightSz) != static_cast<int>(rightSz)) {
      LOG_ERR("SDCF", "Failed to read kern classes");
      freeStyleKernLigatureData(s);
      return false;
    }
  }

  if (hasLig) {
    s.ligaturePairs = new (std::nothrow) EpdLigaturePair[s.header.ligaturePairCount];
    if (!s.ligaturePairs) {
      LOG_ERR("SDCF", "Failed to allocate ligature pairs");
      freeStyleKernLigatureData(s);
      return false;
    }
    if (!file.seekSet(s.ligatureFileOffset)) {
      LOG_ERR("SDCF", "Failed to seek to ligature data");
      freeStyleKernLigatureData(s);
      return false;
    }
    size_t sz = s.header.ligaturePairCount * sizeof(EpdLigaturePair);
    if (file.read(reinterpret_cast<uint8_t*>(s.ligaturePairs), sz) != static_cast<int>(sz)) {
      LOG_ERR("SDCF", "Failed to read ligature pairs");
      freeStyleKernLigatureData(s);
      return false;
    }
  }

  s.kernLigLoaded = true;

  // Make ligatures visible to the stub (used when no mini data built yet).
  // Kern stays nullptr on the stub — it is only wired in miniData via
  // applyKernLigaturePointers() after buildMiniKernMatrix() runs.
  s.stubData.ligaturePairs = s.ligaturePairs;
  s.stubData.ligaturePairCount = s.header.ligaturePairCount;

  LOG_DBG("SDCF", "Kern classes + lig loaded: kernL=%u, kernR=%u, ligs=%u", s.header.kernLeftEntryCount,
          s.header.kernRightEntryCount, s.header.ligaturePairCount);
  return true;
}

// --- Per-page mini kern matrix ---

// Local copy of EpdFont.cpp's lookupKernClass (that one is file-static there).
// Returns the 1-based class ID for `cp`, or 0 if the codepoint has no kerning class.
static uint8_t miniLookupKernClass(const EpdKernClassEntry* entries, uint16_t count, uint32_t cp) {
  if (!entries || count == 0 || cp > 0xFFFF) return 0;
  const auto target = static_cast<uint16_t>(cp);
  const auto* end = entries + count;
  const auto it =
      std::lower_bound(entries, end, target, [](const EpdKernClassEntry& e, uint16_t v) { return e.codepoint < v; });
  return (it != end && it->codepoint == target) ? it->classId : 0;
}

// Build a small per-page kern matrix containing ONLY the (leftClass, rightClass)
// pairs reachable from codepoints in the current text. Class IDs are renumbered
// to a dense 1..N range so the resulting matrix is usedLeft × usedRight (typical
// Latin page: ~25×25 bytes) instead of the font's full ~180×200 (~36KB).
//
// Correctness: EpdFont::getKerning only touches `kernLeftClasses` /
// `kernRightClasses` / `kernMatrix` / the count fields — we swap all of them to
// the mini versions together in applyKernLigaturePointers, so a codepoint not
// on this page simply returns class 0 (no kerning), which was the pre-existing
// behavior for any codepoint outside the kern classes.
bool SdCardFont::buildMiniKernMatrix(PerStyle& s, const uint32_t* codepoints, uint32_t cpCount,
                                     const uint32_t* excluded, uint32_t excludedCount, HalFile& file) {
  freeStyleMiniKern(s);
  // Extra (UI) codepoints are resident but keep the "not on this page" kerning
  // (class 0) they get from the on-demand overflow path.
  const auto kerned = [&](const uint32_t cp) {
    return !excluded || excludedCount == 0 || !std::binary_search(excluded, excluded + excludedCount, cp);
  };
  if (!s.kernLeftClasses || !s.kernRightClasses || s.header.kernLeftEntryCount == 0 ||
      s.header.kernRightEntryCount == 0) {
    return true;  // font has no kern classes — nothing to build
  }

  // Step 1: mark used left/right classes via a 256-wide bitmap (class IDs are uint8_t).
  bool usedLeft[256] = {};
  bool usedRight[256] = {};
  for (uint32_t i = 0; i < cpCount; i++) {
    if (!kerned(codepoints[i])) continue;
    uint8_t lc = miniLookupKernClass(s.kernLeftClasses, s.header.kernLeftEntryCount, codepoints[i]);
    if (lc) usedLeft[lc] = true;
    uint8_t rc = miniLookupKernClass(s.kernRightClasses, s.header.kernRightEntryCount, codepoints[i]);
    if (rc) usedRight[rc] = true;
  }

  // Step 2: build renumber maps (oldClassId -> newClassId, 1-based) and
  // reverse maps (newClassId -> oldClassId) for the SD read step.
  uint8_t leftRenumber[256] = {};
  uint8_t rightRenumber[256] = {};
  uint8_t newToOldLeft[256] = {};
  uint8_t newToOldRight[256] = {};
  uint8_t numLeft = 0, numRight = 0;
  for (int i = 1; i < 256; i++) {
    if (usedLeft[i]) {
      numLeft++;
      leftRenumber[i] = numLeft;
      newToOldLeft[numLeft] = static_cast<uint8_t>(i);
    }
    if (usedRight[i]) {
      numRight++;
      rightRenumber[i] = numRight;
      newToOldRight[numRight] = static_cast<uint8_t>(i);
    }
  }
  if (numLeft == 0 || numRight == 0) {
    return true;  // no kern pairs applicable on this page
  }

  // Step 3: count how many codepoint→classId entries the mini class tables need.
  // Each resident class table has one entry per kerned codepoint in the page.
  uint16_t miniLeftCount = 0;
  uint16_t miniRightCount = 0;
  for (uint32_t i = 0; i < cpCount; i++) {
    if (!kerned(codepoints[i])) continue;
    if (miniLookupKernClass(s.kernLeftClasses, s.header.kernLeftEntryCount, codepoints[i]) != 0) miniLeftCount++;
    if (miniLookupKernClass(s.kernRightClasses, s.header.kernRightEntryCount, codepoints[i]) != 0) miniRightCount++;
  }

  // Step 4: allocate the three mini buffers. The matrix is <1KB in practice
  // (<30 × <30 × 1 byte) so fragmentation is a non-issue.
  const uint32_t matrixBytes = static_cast<uint32_t>(numLeft) * numRight;
  s.miniKernLeftClasses = new (std::nothrow) EpdKernClassEntry[miniLeftCount];
  s.miniKernRightClasses = new (std::nothrow) EpdKernClassEntry[miniRightCount];
  s.miniKernMatrix = new (std::nothrow) int8_t[matrixBytes];
  if (!s.miniKernLeftClasses || !s.miniKernRightClasses || !s.miniKernMatrix) {
    LOG_ERR("SDCF", "Failed to allocate mini kern (%u+%u+%u bytes)", miniLeftCount * 3u, miniRightCount * 3u,
            matrixBytes);
    freeStyleMiniKern(s);
    return false;
  }

  // Step 5: populate mini class tables. `codepoints` is already sorted (see
  // prewarm()) so the output is sorted by codepoint — required for binary
  // search in lookupKernClass during render.
  uint16_t lIdx = 0, rIdx = 0;
  for (uint32_t i = 0; i < cpCount; i++) {
    uint32_t cp = codepoints[i];
    if (cp > 0xFFFF || !kerned(cp)) continue;  // kern class entries are uint16_t
    uint8_t lc = miniLookupKernClass(s.kernLeftClasses, s.header.kernLeftEntryCount, cp);
    if (lc) {
      s.miniKernLeftClasses[lIdx].codepoint = static_cast<uint16_t>(cp);
      s.miniKernLeftClasses[lIdx].classId = leftRenumber[lc];
      lIdx++;
    }
    uint8_t rc = miniLookupKernClass(s.kernRightClasses, s.header.kernRightEntryCount, cp);
    if (rc) {
      s.miniKernRightClasses[rIdx].codepoint = static_cast<uint16_t>(cp);
      s.miniKernRightClasses[rIdx].classId = rightRenumber[rc];
      rIdx++;
    }
  }

  // Step 6: read the full matrix's rows for each used left class, keep only
  // columns for used right classes. One SD seek + one read per used left class;
  // a row is kernRightClassCount bytes (~200 for Literata). Reuses the caller's open handle
  // (the page prewarm's), saving a FAT path walk per page.
  if (!file && !Storage.openFileForRead("SDCF", filePath_, file)) {
    LOG_ERR("SDCF", "Failed to open .cpfont for mini kern: %s", filePath_);
    freeStyleMiniKern(s);
    return false;
  }

  std::unique_ptr<int8_t[]> rowBuf(new (std::nothrow) int8_t[s.header.kernRightClassCount]);
  if (!rowBuf) {
    LOG_ERR("SDCF", "Failed to allocate row buffer (%u bytes)", s.header.kernRightClassCount);
    freeStyleMiniKern(s);
    return false;
  }

  for (uint8_t newL = 1; newL <= numLeft; newL++) {
    const uint8_t oldL = newToOldLeft[newL];
    const uint32_t rowFileOff = s.kernMatrixFileOffset + (oldL - 1u) * s.header.kernRightClassCount;
    if (!file.seekSet(rowFileOff)) {
      LOG_ERR("SDCF", "Failed to seek to kern row %u", oldL);
      freeStyleMiniKern(s);
      return false;
    }
    if (file.read(reinterpret_cast<uint8_t*>(rowBuf.get()), s.header.kernRightClassCount) !=
        static_cast<int>(s.header.kernRightClassCount)) {
      LOG_ERR("SDCF", "Failed to read kern row %u", oldL);
      freeStyleMiniKern(s);
      return false;
    }
    int8_t* miniRow = s.miniKernMatrix + (newL - 1u) * numRight;
    for (uint8_t newR = 1; newR <= numRight; newR++) {
      miniRow[newR - 1] = rowBuf[newToOldRight[newR] - 1u];
    }
  }

  s.miniKernLeftEntryCount = lIdx;
  s.miniKernRightEntryCount = rIdx;
  s.miniKernLeftClassCount = numLeft;
  s.miniKernRightClassCount = numRight;

  LOG_DBG("SDCF", "Built mini kern: %u×%u matrix (%u bytes, full was %u×%u = %u bytes)", numLeft, numRight, matrixBytes,
          s.header.kernLeftClassCount, s.header.kernRightClassCount,
          static_cast<uint32_t>(s.header.kernLeftClassCount) * s.header.kernRightClassCount);
  return true;
}

// --- Glyph miss callback ---

void SdCardFont::applyGlyphMissCallback(uint8_t styleIdx) {
  overflowCtx_[styleIdx].self = this;
  overflowCtx_[styleIdx].styleIdx = styleIdx;

  auto& s = styles_[styleIdx];
  s.stubData.glyphMissHandler = &SdCardFont::onGlyphMiss;
  s.stubData.glyphMissCtx = &overflowCtx_[styleIdx];
  s.stubData.kernLookup = loadMode_ == LoadMode::BoundedUI ? &SdCardFont::lookupBoundedKerning : nullptr;
  s.stubData.ligatureLookup = loadMode_ == LoadMode::BoundedUI ? &SdCardFont::lookupBoundedLigature : nullptr;
}

// --- Compute per-style file offsets from a base data offset ---

void SdCardFont::computeStyleFileOffsets(PerStyle& s, uint32_t baseOffset) {
  s.intervalsFileOffset = baseOffset;
  s.glyphsFileOffset = s.intervalsFileOffset + s.header.intervalCount * sizeof(EpdUnicodeInterval);
  s.kernLeftFileOffset = s.glyphsFileOffset + s.header.glyphCount * sizeof(EpdGlyph);
  s.kernRightFileOffset = s.kernLeftFileOffset + s.header.kernLeftEntryCount * sizeof(EpdKernClassEntry);
  s.kernMatrixFileOffset = s.kernRightFileOffset + s.header.kernRightEntryCount * sizeof(EpdKernClassEntry);
  s.ligatureFileOffset =
      s.kernMatrixFileOffset + static_cast<uint32_t>(s.header.kernLeftClassCount) * s.header.kernRightClassCount;
  s.bitmapFileOffset = s.ligatureFileOffset + s.header.ligaturePairCount * sizeof(EpdLigaturePair);
}

// --- Load ---

bool SdCardFont::load(const char* path, LoadMode mode, void (*progress)()) {
  freeAll();
  loadMode_ = mode;
  progress_ = progress;
  loadedFileSize_ = 0;
  boundedReadFailed_ = false;
  if (!path) return false;
  if (strlen(path) >= sizeof(filePath_)) {
    LOG_ERR("SDCF", "Path too long (%zu bytes, max %zu)", strlen(path), sizeof(filePath_) - 1);
    return false;
  }
  strncpy(filePath_, path, sizeof(filePath_) - 1);
  filePath_[sizeof(filePath_) - 1] = '\0';

  HalFile file;
  if (!Storage.openFileForRead("SDCF", path, file)) {
    LOG_ERR("SDCF", "Failed to open .cpfont: %s", path);
    return false;
  }
  if (file.size() > UINT32_MAX) return false;
  loadedFileSize_ = static_cast<uint32_t>(file.size());

  // Read and validate global header
  uint8_t headerBuf[HEADER_SIZE];
  if (file.read(headerBuf, HEADER_SIZE) != HEADER_SIZE) {
    LOG_ERR("SDCF", "Failed to read header");
    return false;
  }

  if (memcmp(headerBuf, CPFONT_MAGIC, 8) != 0) {
    LOG_ERR("SDCF", "Invalid magic bytes");
    return false;
  }

  uint16_t fileVersion = readU16(headerBuf + 8);
  if (fileVersion != CPFONT_VERSION) {
    LOG_ERR("SDCF", "Unsupported version: %u (expected %u)", fileVersion, CPFONT_VERSION);
    return false;
  }

  // Begin content hash: accumulate global header
  uint32_t hash = fnv1a(headerBuf, HEADER_SIZE);

  bool is2Bit = (readU16(headerBuf + 10) & 1) != 0;

  uint8_t styleCount = headerBuf[12];
  if (styleCount == 0 || styleCount > MAX_STYLES) {
    LOG_ERR("SDCF", "Invalid style count: %u", styleCount);
    return false;
  }

  // Read style TOC
  for (uint8_t i = 0; i < styleCount; i++) {
    uint8_t tocBuf[STYLE_TOC_ENTRY_SIZE];
    if (file.read(tocBuf, STYLE_TOC_ENTRY_SIZE) != STYLE_TOC_ENTRY_SIZE) {
      LOG_ERR("SDCF", "Failed to read style TOC entry %u", i);
      freeAll();
      return false;
    }

    // Accumulate TOC entry into content hash
    hash = fnv1a(tocBuf, STYLE_TOC_ENTRY_SIZE, hash);

    uint8_t styleId = tocBuf[0];
    if (styleId >= MAX_STYLES) {
      LOG_ERR("SDCF", "Invalid styleId %u in TOC", styleId);
      file.close();
      freeAll();
      return false;
    }

    auto& s = styles_[styleId];
    if (s.present) {
      LOG_ERR("SDCF", "Duplicate style %u", styleId);
      freeAll();
      return false;
    }
    s.present = true;
    s.header.intervalCount = readU32(tocBuf + 4);
    s.header.glyphCount = readU32(tocBuf + 8);
    s.header.advanceY = tocBuf[12];
    s.header.ascender = readI16(tocBuf + 13);
    s.header.descender = readI16(tocBuf + 15);
    s.header.kernLeftEntryCount = readU16(tocBuf + 17);
    s.header.kernRightEntryCount = readU16(tocBuf + 19);
    s.header.kernLeftClassCount = tocBuf[21];
    s.header.kernRightClassCount = tocBuf[22];
    s.header.ligaturePairCount = tocBuf[23];
    s.header.is2Bit = is2Bit;

    // Sanity-check counts to reject malformed files before allocating.
    // Kern class counts are uint8 (bounded by type). Entry counts are uint16
    // but in practice a sane font has far fewer than 4096 per-side kern entries.
    static constexpr uint32_t MAX_INTERVALS = 4096;
    static constexpr uint32_t MAX_GLYPHS = 65536;
    static constexpr uint32_t MAX_KERN_ENTRIES = 4096;
    if (s.header.intervalCount > MAX_INTERVALS || s.header.glyphCount > MAX_GLYPHS ||
        s.header.kernLeftEntryCount > MAX_KERN_ENTRIES || s.header.kernRightEntryCount > MAX_KERN_ENTRIES) {
      LOG_ERR("SDCF", "Style %u: unreasonable counts (iv=%u, gl=%u, kL=%u, kR=%u)", styleId, s.header.intervalCount,
              s.header.glyphCount, s.header.kernLeftEntryCount, s.header.kernRightEntryCount);
      file.close();
      freeAll();
      return false;
    }

    uint32_t dataOffset = readU32(tocBuf + 24);
    // Counts are bounded above. Use wide arithmetic before the existing
    // offset builder so a corrupt base cannot wrap into the file header.
    const uint64_t metadataEnd =
        static_cast<uint64_t>(dataOffset) + s.header.intervalCount * sizeof(EpdUnicodeInterval) +
        s.header.glyphCount * sizeof(EpdGlyph) +
        (s.header.kernLeftEntryCount + s.header.kernRightEntryCount) * sizeof(EpdKernClassEntry) +
        static_cast<uint32_t>(s.header.kernLeftClassCount) * s.header.kernRightClassCount +
        s.header.ligaturePairCount * sizeof(EpdLigaturePair);
    if (dataOffset < HEADER_SIZE + styleCount * STYLE_TOC_ENTRY_SIZE || metadataEnd > loadedFileSize_) {
      LOG_ERR("SDCF", "Font metadata outside file");
      freeAll();
      return false;
    }
    computeStyleFileOffsets(s, dataOffset);
  }

  styleCount_ = styleCount;
  contentHash_ = hash;

  // Load full intervals into RAM for each present style. BMP-only fonts with
  // fewer than 65536 glyphs use a compact 6-byte interval table instead of the
  // on-disk 12-byte table; large sparse CJK subsets otherwise keep tens of KB
  // of always-resident heap just for lookup metadata.
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    auto& s = styles_[i];
    if (!s.present) continue;

    if (!file.seekSet(s.intervalsFileOffset)) {
      LOG_ERR("SDCF", "Failed to seek to intervals for style %u", i);
      freeAll();
      return false;
    }

    // Validate interval contents before any later code (findGlobalGlyphIndex,
    // glyph reads) trusts them. A malformed file could otherwise drive
    // out-of-range glyph indices into bogus on-disk reads.
    bool canUseBmp16 = s.header.glyphCount <= UINT16_MAX;
    uint32_t expectedOffset = 0;
    uint32_t prevLast = 0;
    EpdUnicodeInterval iv{};
    for (uint32_t j = 0; j < s.header.intervalCount; ++j) {
      if (progress_) progress_();
      if (file.read(reinterpret_cast<uint8_t*>(&iv), sizeof(iv)) != sizeof(iv)) {
        LOG_ERR("SDCF", "Failed to read interval %u for style %u", j, i);
        freeAll();
        return false;
      }
      if (iv.first > iv.last || iv.last > 0x10FFFF) {
        LOG_ERR("SDCF", "Style %u: invalid interval %u (first 0x%lX > last 0x%lX)", i, j,
                static_cast<unsigned long>(iv.first), static_cast<unsigned long>(iv.last));
        file.close();
        freeAll();
        return false;
      }
      const uint32_t span = iv.last - iv.first + 1;
      const bool overlapsPrev = (j > 0 && iv.first <= prevLast);
      const bool spanTooBig = (span > s.header.glyphCount);
      const bool offsetMismatch = (iv.offset != expectedOffset);
      const bool offsetOverruns = (iv.offset > s.header.glyphCount - span);
      if (overlapsPrev || spanTooBig || offsetMismatch || offsetOverruns) {
        LOG_ERR("SDCF", "Style %u: invalid interval layout at %u (overlap=%d span=%u offMis=%d offOver=%d)", i, j,
                overlapsPrev, span, offsetMismatch, offsetOverruns);
        file.close();
        freeAll();
        return false;
      }
      if (iv.first > UINT16_MAX || iv.last > UINT16_MAX || iv.offset > UINT16_MAX) {
        canUseBmp16 = false;
      }
      expectedOffset += span;
      prevLast = iv.last;
    }

    if (!file.seekSet(s.intervalsFileOffset)) {
      LOG_ERR("SDCF", "Failed to seek back to intervals for style %u", i);
      freeAll();
      return false;
    }

    if (mode == LoadMode::BoundedUI) {
      // Validated above; on-demand binary search reads one 12B interval.
      // No resident table or extra permanent scratch buffer.
    } else if (canUseBmp16) {
      s.bmpIntervals = new (std::nothrow) PerStyle::BmpInterval16[s.header.intervalCount];
      if (!s.bmpIntervals) {
        LOG_ERR("SDCF", "Failed to allocate compact intervals for style %u", i);
        freeAll();
        return false;
      }
      for (uint32_t j = 0; j < s.header.intervalCount; ++j) {
        if (file.read(reinterpret_cast<uint8_t*>(&iv), sizeof(iv)) != sizeof(iv)) {
          LOG_ERR("SDCF", "Failed to read compact interval %u for style %u", j, i);
          freeAll();
          return false;
        }
        s.bmpIntervals[j] = {static_cast<uint16_t>(iv.first), static_cast<uint16_t>(iv.last),
                             static_cast<uint16_t>(iv.offset)};
      }
      s.intervalsAreBmp16 = true;
    } else {
      s.fullIntervals = new (std::nothrow) EpdUnicodeInterval[s.header.intervalCount];
      if (!s.fullIntervals) {
        LOG_ERR("SDCF", "Failed to allocate %u intervals for style %u", s.header.intervalCount, i);
        freeAll();
        return false;
      }
      size_t intervalsBytes = s.header.intervalCount * sizeof(EpdUnicodeInterval);
      if (file.read(reinterpret_cast<uint8_t*>(s.fullIntervals), intervalsBytes) != static_cast<int>(intervalsBytes)) {
        LOG_ERR("SDCF", "Failed to read intervals for style %u", i);
        freeAll();
        return false;
      }
    }

    // Initialize stub data
    memset(&s.stubData, 0, sizeof(s.stubData));
    s.stubData.advanceY = s.header.advanceY;
    s.stubData.ascender = s.header.ascender;
    s.stubData.descender = s.header.descender;
    s.stubData.is2Bit = s.header.is2Bit;

    s.epdFont.data = &s.stubData;
    applyGlyphMissCallback(i);
  }

  loaded_ = true;

  LOG_DBG("SDCF", "Loaded: %s (v%u, %u styles)", path, CPFONT_VERSION, styleCount_);
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    if (!styles_[i].present) continue;
    const auto& h = styles_[i].header;
    LOG_DBG("SDCF", "  style[%u]: %u intervals, %u glyphs, advY=%u, asc=%d, desc=%d, kernL=%u, kernR=%u, ligs=%u", i,
            h.intervalCount, h.glyphCount, h.advanceY, h.ascender, h.descender, h.kernLeftEntryCount,
            h.kernRightEntryCount, h.ligaturePairCount);
  }
  return true;
}

// --- Codepoint lookup ---

int32_t SdCardFont::findGlobalGlyphIndex(const PerStyle& s, uint32_t codepoint) const {
  HalFile file;
  if (loadMode_ == LoadMode::BoundedUI && !Storage.openFileForRead("SDCF", filePath_, file)) {
    boundedReadFailed_ = true;
    return -1;
  }
  int left = 0;
  int right = static_cast<int>(s.header.intervalCount) - 1;
  while (left <= right) {
    int mid = left + (right - left) / 2;
    EpdUnicodeInterval interval{};
    if (loadMode_ == LoadMode::BoundedUI) {
      if (progress_) progress_();
      const size_t position = s.intervalsFileOffset + static_cast<size_t>(mid) * sizeof(interval);
      if (!file.seekSet(position) || file.read(&interval, sizeof(interval)) != sizeof(interval) ||
          interval.first > interval.last || interval.last > 0x10FFFF || interval.offset >= s.header.glyphCount ||
          interval.last - interval.first >= s.header.glyphCount - interval.offset) {
        boundedReadFailed_ = true;
        return -1;
      }
    } else if (s.intervalsAreBmp16) {
      interval = {s.bmpIntervals[mid].first, s.bmpIntervals[mid].last, s.bmpIntervals[mid].offset};
    } else {
      interval = s.fullIntervals[mid];
    }
    const uint32_t first = interval.first;
    const uint32_t last = interval.last;
    if (codepoint < first) {
      right = mid - 1;
    } else if (codepoint > last) {
      left = mid + 1;
    } else {
      const uint32_t offset = interval.offset;
      return static_cast<int32_t>(offset + (codepoint - first));
    }
  }
  return -1;
}

// --- Prewarm ---

int SdCardFont::prewarm(const char* utf8Text, uint8_t styleMask, bool metadataOnly, const char* extraText) {
  if (!loaded_) return -1;
  if (loadMode_ == LoadMode::BoundedUI) return checkBoundedText(utf8Text, styleMask);
  styleMask = resolveStyleMask(styleMask);
  if (styleMask == 0) return 0;

  unsigned long startMs = millis();

  // Step 1: Extract unique codepoints from UTF-8 text (shared across all styles).
  // Dedup uses O(n^2) linear scan — worst case is MAX_PAGE_GLYPHS (512) unique codepoints
  // = ~131K comparisons, but in practice pages contain far fewer unique codepoints so the
  // actual cost is much lower. This is dwarfed by SD I/O that follows. Alternatives (hash
  // set, bitmap) exceed the 256-byte stack limit or add template bloat.
  // Heap-allocated, too large for the stack (limit < 256 bytes). Sized to what this text can
  // need, at most MAX_PAGE_GLYPHS * 4 = 2048 bytes: a CJK page decodes to a few hundred
  // codepoints, and this buffer is live at the page render's prewarm memory peak.
  uint32_t capacity = 1;  // replacement glyph
  for (const unsigned char* q = reinterpret_cast<const unsigned char*>(utf8Text);
       capacity < MAX_PAGE_GLYPHS && utf8NextCodepoint(&q) != 0;) {
    capacity++;
  }
  if (extraText && !metadataOnly) capacity += MAX_EXTRA_GLYPHS;
  if (!metadataOnly) {
    for (uint8_t si = 0; si < MAX_STYLES; si++) {
      if ((styleMask & (1 << si)) && styles_[si].present) capacity += styles_[si].header.ligaturePairCount;
    }
  }
  if (capacity > MAX_PAGE_GLYPHS) capacity = MAX_PAGE_GLYPHS;
  std::unique_ptr<uint32_t[]> codepoints(new (std::nothrow) uint32_t[capacity]);
  if (!codepoints) {
    LOG_ERR("SDCF", "Failed to allocate codepoint buffer (%u bytes)", static_cast<unsigned>(capacity * 4));
    return -1;
  }
  uint32_t cpCount = 0;

  const unsigned char* p = reinterpret_cast<const unsigned char*>(utf8Text);
  while (*p && cpCount < capacity) {
    uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0) break;

    bool found = false;
    for (uint32_t i = 0; i < cpCount; i++) {
      if (codepoints[i] == cp) {
        found = true;
        break;
      }
    }
    if (!found) {
      codepoints[cpCount++] = cp;
    }
  }

  // Always include the replacement character
  {
    bool hasReplacement = false;
    for (uint32_t i = 0; i < cpCount; i++) {
      if (codepoints[i] == REPLACEMENT_GLYPH) {
        hasReplacement = true;
        break;
      }
    }
    if (!hasReplacement && cpCount < capacity) {
      codepoints[cpCount++] = REPLACEMENT_GLYPH;
    }
  }

  // Add ligature output codepoints from all styles being prewarmed.
  // Skip during metadata-only prewarm (layout measurement) to avoid loading
  // kern/lig data for all styles upfront (~22KB per style). Kern/lig is
  // loaded per-style in prewarmStyle() during the full render prewarm instead.
  if (!metadataOnly) {
    for (uint8_t si = 0; si < MAX_STYLES; si++) {
      if (!(styleMask & (1 << si)) || !styles_[si].present) continue;
      auto& s = styles_[si];

      loadStyleKernLigatureData(s);
      if (s.ligaturePairs && s.header.ligaturePairCount > 0) {
        for (uint8_t li = 0; li < s.header.ligaturePairCount && cpCount < capacity; li++) {
          uint32_t leftCp = s.ligaturePairs[li].pair >> 16;
          uint32_t rightCp = s.ligaturePairs[li].pair & 0xFFFF;
          uint32_t outCp = s.ligaturePairs[li].ligatureCp;

          bool hasLeft = false, hasRight = false;
          for (uint32_t i = 0; i < cpCount; i++) {
            if (codepoints[i] == leftCp) hasLeft = true;
            if (codepoints[i] == rightCp) hasRight = true;
            if (hasLeft && hasRight) break;
          }
          if (!hasLeft || !hasRight) continue;

          bool hasOut = false;
          for (uint32_t i = 0; i < cpCount; i++) {
            if (codepoints[i] == outCp) {
              hasOut = true;
              break;
            }
          }
          if (!hasOut) {
            codepoints[cpCount++] = outCp;
          }
        }
      }
    }
  }

  // Extra UI codepoints go last, so page glyphs keep priority under MAX_PAGE_GLYPHS.
  // Stack array: MAX_EXTRA_GLYPHS * 4 = 128 bytes.
  uint32_t extras[MAX_EXTRA_GLYPHS];
  uint32_t extraCount = 0;
  if (extraText && !metadataOnly) {
    const unsigned char* e = reinterpret_cast<const unsigned char*>(extraText);
    while (*e && cpCount < capacity && extraCount < MAX_EXTRA_GLYPHS) {
      const uint32_t cp = utf8NextCodepoint(&e);
      if (cp == 0) break;
      bool onPage = false;
      for (uint32_t i = 0; i < cpCount && !onPage; i++) onPage = codepoints[i] == cp;
      if (onPage) continue;
      codepoints[cpCount++] = cp;
      extras[extraCount++] = cp;
    }
    std::sort(extras, extras + extraCount);
  }

  // Sort codepoints for ordered interval building
  std::sort(codepoints.get(), codepoints.get() + cpCount);

  // Prewarm each requested style
  int totalMissed = 0;
  for (uint8_t si = 0; si < MAX_STYLES; si++) {
    if (!(styleMask & (1 << si)) || !styles_[si].present) continue;
    totalMissed += prewarmStyle(si, codepoints.get(), cpCount, metadataOnly, extras, extraCount);
  }

  stats_.prewarmTotalMs = millis() - startMs;
  return totalMissed;
}

int SdCardFont::prewarmStyle(uint8_t styleIdx, const uint32_t* codepoints, uint32_t cpCount, bool metadataOnly,
                             const uint32_t* kernExcluded, uint32_t kernExcludedCount) {
  auto& s = styles_[styleIdx];

  // Map codepoints to global glyph indices for this style
  struct CpGlyphMapping {
    uint32_t codepoint;
    int32_t globalIndex;
  };
  CpGlyphMapping* mappings = new (std::nothrow) CpGlyphMapping[cpCount];
  if (!mappings) {
    LOG_ERR("SDCF", "Failed to allocate mapping array for style %u", styleIdx);
    return static_cast<int>(cpCount);
  }

  uint32_t validCount = 0;
  for (uint32_t i = 0; i < cpCount; i++) {
    int32_t idx = findGlobalGlyphIndex(s, codepoints[i]);
    if (idx >= 0) {
      mappings[validCount].codepoint = codepoints[i];
      mappings[validCount].globalIndex = idx;
      validCount++;
    }
  }
  int missed = static_cast<int>(cpCount - validCount);

  if (validCount == 0) {
    freeStyleMiniData(s);
    delete[] mappings;
    s.epdFont.data = &s.stubData;
    return missed;
  }

  // Build mini intervals from sorted codepoints
  freeStyleMiniData(s);

  // One interval per run of consecutive codepoints (the table stays resident for the page).
  uint32_t intervalCapacity = 1;
  for (uint32_t i = 1; i < validCount; i++) {
    if (mappings[i].codepoint != mappings[i - 1].codepoint + 1) intervalCapacity++;
  }
  s.miniIntervals = new (std::nothrow) EpdUnicodeInterval[intervalCapacity];
  if (!s.miniIntervals) {
    LOG_ERR("SDCF", "Failed to allocate mini intervals for style %u", styleIdx);
    delete[] mappings;
    return static_cast<int>(cpCount);
  }

  s.miniIntervalCount = 0;
  uint32_t rangeStart = 0;
  for (uint32_t i = 1; i <= validCount; i++) {
    if (i == validCount || mappings[i].codepoint != mappings[i - 1].codepoint + 1) {
      s.miniIntervals[s.miniIntervalCount].first = mappings[rangeStart].codepoint;
      s.miniIntervals[s.miniIntervalCount].last = mappings[i - 1].codepoint;
      s.miniIntervals[s.miniIntervalCount].offset = rangeStart;
      s.miniIntervalCount++;
      rangeStart = i;
    }
  }

  // Allocate mini glyph array
  s.miniGlyphCount = validCount;
  s.miniGlyphs = new (std::nothrow) EpdGlyph[s.miniGlyphCount];
  if (!s.miniGlyphs) {
    LOG_ERR("SDCF", "Failed to allocate mini glyphs for style %u", styleIdx);
    delete[] mappings;
    freeStyleMiniData(s);
    return static_cast<int>(cpCount);
  }

  // Build sorted read order for sequential I/O
  uint32_t* readOrder = new (std::nothrow) uint32_t[validCount];
  if (!readOrder) {
    LOG_ERR("SDCF", "Failed to allocate read order for style %u", styleIdx);
    delete[] mappings;
    freeStyleMiniData(s);
    return static_cast<int>(cpCount);
  }
  for (uint32_t i = 0; i < validCount; i++) readOrder[i] = i;
  std::sort(readOrder, readOrder + validCount,
            [&](uint32_t a, uint32_t b) { return mappings[a].globalIndex < mappings[b].globalIndex; });

  HalFile file;
  if (!Storage.openFileForRead("SDCF", filePath_, file)) {
    LOG_ERR("SDCF", "Failed to reopen .cpfont for prewarm (style %u)", styleIdx);
    delete[] readOrder;
    delete[] mappings;
    freeStyleMiniData(s);
    return static_cast<int>(cpCount);
  }

  unsigned long sdStart = millis();
  uint32_t seekCount = 0;

  // Read glyph metadata. lastReadIndex tracks sequential reads to skip redundant
  // seeks; INT32_MIN guarantees the first iteration always seeks to the correct
  // offset (otherwise when gIdx == 0, the "gIdx != lastReadIndex + 1" check would
  // be false and we'd read from the file's current position — the header — which
  // decodes to a garbage EpdGlyph with a massive advanceX, inflating any word
  // containing that codepoint beyond page width).
  int32_t lastReadIndex = INT32_MIN;
  for (uint32_t i = 0; i < validCount; i++) {
    uint32_t mapIdx = readOrder[i];
    int32_t gIdx = mappings[mapIdx].globalIndex;

    uint32_t fileOff = s.glyphsFileOffset + static_cast<uint32_t>(gIdx) * sizeof(EpdGlyph);
    if (gIdx != lastReadIndex + 1) {
      if (!file.seekSet(fileOff)) {
        LOG_ERR("SDCF", "Prewarm: failed to seek to glyph %d (style %u)", gIdx, styleIdx);
        file.close();
        delete[] readOrder;
        delete[] mappings;
        freeStyleMiniData(s);
        return static_cast<int>(cpCount);
      }
      seekCount++;
    }
    if (file.read(reinterpret_cast<uint8_t*>(&s.miniGlyphs[mapIdx]), sizeof(EpdGlyph)) != sizeof(EpdGlyph)) {
      LOG_ERR("SDCF", "Prewarm: short glyph read (style %u, glyph %d)", styleIdx, gIdx);
      delete[] readOrder;
      delete[] mappings;
      freeStyleMiniData(s);
      return static_cast<int>(cpCount);
    }
    lastReadIndex = gIdx;
  }

  uint32_t totalBitmapSize = 0;

  if (!metadataOnly) {
    // Compute total bitmap size
    for (uint32_t i = 0; i < validCount; i++) {
      totalBitmapSize += s.miniGlyphs[i].dataLength;
    }

    s.miniBitmap = new (std::nothrow) uint8_t[totalBitmapSize > 0 ? totalBitmapSize : 1];
    if (!s.miniBitmap) {
      LOG_ERR("SDCF", "Failed to allocate mini bitmap (%u bytes) for style %u", totalBitmapSize, styleIdx);
      delete[] readOrder;
      delete[] mappings;
      freeStyleMiniData(s);
      return static_cast<int>(cpCount);
    }

    // Read bitmap data sorted by file offset
    std::sort(readOrder, readOrder + validCount,
              [&](uint32_t a, uint32_t b) { return s.miniGlyphs[a].dataOffset < s.miniGlyphs[b].dataOffset; });

    uint32_t miniBitmapOffset = 0;
    uint32_t lastBitmapEnd = UINT32_MAX;
    for (uint32_t i = 0; i < validCount; i++) {
      uint32_t mapIdx = readOrder[i];
      EpdGlyph& glyph = s.miniGlyphs[mapIdx];

      if (glyph.dataLength == 0) {
        glyph.dataOffset = miniBitmapOffset;
        continue;
      }

      uint32_t fileOff = s.bitmapFileOffset + glyph.dataOffset;
      if (fileOff != lastBitmapEnd) {
        if (!file.seekSet(fileOff)) {
          LOG_ERR("SDCF", "Prewarm: failed to seek to bitmap (style %u)", styleIdx);
          file.close();
          delete[] readOrder;
          delete[] mappings;
          freeStyleMiniData(s);
          return static_cast<int>(cpCount);
        }
        seekCount++;
      }
      if (file.read(s.miniBitmap + miniBitmapOffset, glyph.dataLength) != static_cast<int>(glyph.dataLength)) {
        LOG_ERR("SDCF", "Prewarm: short bitmap read (style %u)", styleIdx);
        delete[] readOrder;
        delete[] mappings;
        freeStyleMiniData(s);
        return static_cast<int>(cpCount);
      }
      lastBitmapEnd = fileOff + glyph.dataLength;

      glyph.dataOffset = miniBitmapOffset;
      miniBitmapOffset += glyph.dataLength;
    }
  }

  uint32_t sdTime = millis() - sdStart;
  delete[] readOrder;
  delete[] mappings;

  // Full render prewarm: load the persistent kern classes + ligatures (one-time
  // per style, small — the big matrix is NOT loaded here) and then build the
  // per-page mini kern matrix restricted to class pairs reachable from this
  // page's codepoints. Skip during metadata-only prewarm — layout only needs
  // advanceX and the mini kern would be thrown away before rendering.
  bool kernLigOk = false;
  if (!metadataOnly) {
    if (loadStyleKernLigatureData(s)) {
      kernLigOk = buildMiniKernMatrix(s, codepoints, cpCount, kernExcluded, kernExcludedCount, file);
    }
  }

  // Populate miniData and swap
  memset(&s.miniData, 0, sizeof(s.miniData));
  s.miniData.bitmap = s.miniBitmap;
  s.miniData.glyph = s.miniGlyphs;
  s.miniData.intervals = s.miniIntervals;
  s.miniData.intervalCount = s.miniIntervalCount;
  s.miniData.advanceY = s.header.advanceY;
  s.miniData.ascender = s.header.ascender;
  s.miniData.descender = s.header.descender;
  s.miniData.is2Bit = s.header.is2Bit;
  if (kernLigOk) {
    applyKernLigaturePointers(s, s.miniData);
  }
  s.miniData.glyphMissHandler = &SdCardFont::onGlyphMiss;
  s.miniData.glyphMissCtx = &overflowCtx_[styleIdx];

  s.epdFont.data = &s.miniData;

  // Accumulate stats
  stats_.sdReadTimeMs += sdTime;
  stats_.seekCount += seekCount;
  stats_.uniqueGlyphs += validCount;
  stats_.bitmapBytes += totalBitmapSize;

  return missed;
}

// --- Cache management ---

void SdCardFont::clearCache() {
  clearOverflow();
  // Note: advance table is intentionally preserved here. It persists across
  // layout passes so repeated section indexing amortizes SD reads; a table a
  // large request grew past ADVANCE_CACHE_LIMIT is trimmed back now that the
  // request is over. Use clearPersistentCache() to wipe it.
  trimAdvanceCache();
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    if (!styles_[i].present) continue;
    freeStyleMiniData(styles_[i]);
    applyGlyphMissCallback(i);
  }
}

// --- Advance table ---

void SdCardFont::clearPersistentCache() {
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    delete[] advance_[i].entries;
    advance_[i] = AdvanceTable{};
    delete uniform_[i];
    uniform_[i] = nullptr;
  }
  advanceGeneration_ = 0;
}

uint16_t SdCardFont::beginAdvanceRequest() {
  if (advanceGeneration_ == UINT16_MAX) {
    // Generation counter would wrap: age every entry equally so the modular
    // age comparison below stays meaningful. Only eviction order is affected.
    for (auto& table : advance_) {
      for (uint32_t i = 0; i < table.size; i++) table.entries[i].lastUse = 0;
    }
    advanceGeneration_ = 1;
  } else {
    advanceGeneration_++;
  }
  return advanceGeneration_;
}

SdCardFont::AdvanceEntry* SdCardFont::findAdvanceEntry(uint8_t styleIdx, uint32_t codepoint) const {
  const AdvanceTable& table = advance_[styleIdx & (MAX_STYLES - 1)];
  if (!table.entries || table.size == 0) return nullptr;
  uint32_t lo = 0, hi = table.size;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (table.entries[mid].codepoint < codepoint) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo < table.size && table.entries[lo].codepoint == codepoint) return &table.entries[lo];
  return nullptr;
}

bool SdCardFont::growAdvanceTable(uint8_t styleIdx, uint32_t newCapacity) {
  AdvanceTable& table = advance_[styleIdx];
  if (newCapacity <= table.capacity) return true;
  // One allocation per growth step (geometric up to the steady-state limit),
  // not per request: the previous design reallocated on every merge.
  auto* grown = new (std::nothrow) AdvanceEntry[newCapacity];
  if (!grown) {
    LOG_ERR("SDCF", "Advance table: alloc failed (%u entries) style %u", newCapacity, styleIdx);
    return false;
  }
  if (table.size > 0) memcpy(grown, table.entries, table.size * sizeof(AdvanceEntry));
  delete[] table.entries;
  table.entries = grown;
  table.capacity = newCapacity;
  return true;
}

// Removes up to `count` entries whose generation differs from
// `protectedGeneration`, oldest generation first. Compaction keeps the table
// sorted. Returns the number of entries removed.
uint32_t SdCardFont::evictStaleAdvances(uint8_t styleIdx, uint32_t count, uint16_t protectedGeneration) {
  AdvanceTable& table = advance_[styleIdx];
  uint32_t removed = 0;
  while (removed < count && table.size > 0) {
    bool found = false;
    uint16_t oldest = 0;
    uint16_t oldestAge = 0;
    for (uint32_t i = 0; i < table.size; i++) {
      const uint16_t use = table.entries[i].lastUse;
      if (use == protectedGeneration) continue;
      const auto age = static_cast<uint16_t>(protectedGeneration - use);
      if (!found || age > oldestAge) {
        found = true;
        oldest = use;
        oldestAge = age;
      }
    }
    if (!found) break;
    // Remove only as many of the (equally old) entries as still needed.
    uint32_t kept = 0;
    for (uint32_t i = 0; i < table.size; i++) {
      if (table.entries[i].lastUse == oldest && removed < count) {
        removed++;
        continue;
      }
      table.entries[kept++] = table.entries[i];
    }
    table.size = kept;
  }
  stats_.advanceEvictions += removed;
  return removed;
}

// Makes room for `needed` new entries and returns how many fit. Growth stays
// geometric up to ADVANCE_CACHE_LIMIT; past it, stale entries are evicted;
// only a request whose own working set exceeds the limit grows the table
// further, up to ADVANCE_REQUEST_LIMIT.
uint32_t SdCardFont::reserveAdvanceSlots(uint8_t styleIdx, uint32_t needed, uint16_t generation) {
  const AdvanceTable& table = advance_[styleIdx];  // grown/evicted through the helpers below
  const uint32_t target = table.size + needed;
  if (target > table.capacity && table.capacity < ADVANCE_CACHE_LIMIT) {
    uint32_t grown = table.capacity ? table.capacity * 2 : ADVANCE_INITIAL_CAPACITY;
    if (grown < target) grown = target;
    if (grown > ADVANCE_CACHE_LIMIT) grown = ADVANCE_CACHE_LIMIT;
    growAdvanceTable(styleIdx, grown);
  }
  if (target > table.capacity) evictStaleAdvances(styleIdx, target - table.capacity, generation);
  if (table.size + needed > table.capacity && table.capacity < ADVANCE_REQUEST_LIMIT) {
    uint32_t grown = table.size + needed;
    if (grown > ADVANCE_REQUEST_LIMIT) grown = ADVANCE_REQUEST_LIMIT;
    growAdvanceTable(styleIdx, grown);
  }
  const uint32_t room = table.capacity - table.size;
  return needed < room ? needed : room;
}

// Merges sorted, non-overlapping new entries into the sorted table in place
// (from the back). The caller reserved the slots.
void SdCardFont::insertAdvances(uint8_t styleIdx, const AdvanceEntry* sortedNew, uint32_t count) {
  AdvanceTable& table = advance_[styleIdx];
  if (count == 0 || table.size + count > table.capacity) return;
  int64_t i = static_cast<int64_t>(table.size) - 1;
  int64_t j = static_cast<int64_t>(count) - 1;
  int64_t k = static_cast<int64_t>(table.size + count) - 1;
  while (j >= 0) {
    if (i >= 0 && table.entries[i].codepoint > sortedNew[j].codepoint) {
      table.entries[k--] = table.entries[i--];
    } else {
      table.entries[k--] = sortedNew[j--];
    }
  }
  table.size += count;
}

void SdCardFont::trimAdvanceCache() {
  for (uint8_t si = 0; si < MAX_STYLES; si++) {
    AdvanceTable& table = advance_[si];
    if (table.capacity <= ADVANCE_CACHE_LIMIT) continue;
    // Nothing is protected: the request that grew the table has finished.
    if (table.size > ADVANCE_CACHE_LIMIT) {
      evictStaleAdvances(si, table.size - ADVANCE_CACHE_LIMIT, static_cast<uint16_t>(advanceGeneration_ + 1));
    }
    auto* shrunk = new (std::nothrow) AdvanceEntry[ADVANCE_CACHE_LIMIT];
    if (!shrunk) continue;  // keep the larger table; it is still correct
    memcpy(shrunk, table.entries, table.size * sizeof(AdvanceEntry));
    delete[] table.entries;
    table.entries = shrunk;
    table.capacity = ADVANCE_CACHE_LIMIT;
  }
}

bool SdCardFont::lookupAdvance(uint32_t codepoint, uint8_t style, uint16_t* outAdvance) const {
  const uint8_t styleIdx = style & (MAX_STYLES - 1);
  if (const AdvanceEntry* entry = findAdvanceEntry(styleIdx, codepoint)) {
    if (outAdvance) *outAdvance = entry->advanceX;
    return true;
  }
  if (const AdvanceRun* run = findUniformAdvance(styleIdx, codepoint)) {
    if (outAdvance) *outAdvance = run->advanceX;
    return true;
  }
  return false;
}

bool SdCardFont::hasAdvanceTable() const {
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    if (advance_[i].entries || (uniform_[i] && uniform_[i]->runCount > 0)) return true;
  }
  return false;
}

const SdCardFont::AdvanceRun* SdCardFont::findUniformAdvance(uint8_t styleIdx, uint32_t codepoint) const {
  const UniformAdvances* uniform = uniform_[styleIdx & (MAX_STYLES - 1)];
  if (!uniform) return nullptr;
  for (uint8_t i = 0; i < uniform->runCount; i++) {
    if (codepoint >= uniform->runs[i].first && codepoint <= uniform->runs[i].last) return &uniform->runs[i];
  }
  return nullptr;
}

bool SdCardFont::intervalAt(const PerStyle& s, uint32_t index, EpdUnicodeInterval* out) {
  if (index >= s.header.intervalCount) return false;
  if (s.intervalsAreBmp16 && s.bmpIntervals) {
    *out = {s.bmpIntervals[index].first, s.bmpIntervals[index].last, s.bmpIntervals[index].offset};
    return true;
  }
  if (s.fullIntervals) {
    *out = s.fullIntervals[index];
    return true;
  }
  return false;  // BoundedUI keeps intervals on SD
}

int32_t SdCardFont::findIntervalIndex(const PerStyle& s, uint32_t codepoint) {
  int left = 0;
  int right = static_cast<int>(s.header.intervalCount) - 1;
  EpdUnicodeInterval interval{};
  while (left <= right) {
    const int mid = left + (right - left) / 2;
    if (!intervalAt(s, static_cast<uint32_t>(mid), &interval)) return -1;
    if (codepoint < interval.first) {
      right = mid - 1;
    } else if (codepoint > interval.last) {
      left = mid + 1;
    } else {
      return mid;
    }
  }
  return -1;
}

uint16_t SdCardFont::getAdvance(uint32_t codepoint, uint8_t style) const {
  uint16_t advance = 0;
  return lookupAdvance(codepoint, style, &advance) ? advance : 0;
}

bool SdCardFont::hasGlyph(uint32_t codepoint, uint8_t style) const {
  if (!loaded_) return false;
  const uint8_t styleIdx = resolveStyle(style);
  const auto& s = styles_[styleIdx];
  if (!s.present) return false;
  if (loadMode_ == LoadMode::BoundedUI) {
    if (recentlyMissed(codepoint, styleIdx)) return false;
    const bool found = findGlobalGlyphIndex(s, codepoint) >= 0;
    if (!found && !boundedReadFailed_) rememberMiss(codepoint, styleIdx);
    return found;
  }
  return findGlobalGlyphIndex(s, codepoint) >= 0;
}

// One layout request: collects the codepoints of one style that are not yet
// resident, and fetches them in batches of ADVANCE_FETCH_BATCH with one SD open
// per batch. Scratch buffers (~5 KiB) are allocated on the first miss and live
// for the request only, so a paragraph whose advances are all resident (the
// steady state while paging through a chapter) allocates nothing.
struct SdCardFont::AdvanceRequest {
  struct CpIdx {
    uint32_t codepoint;
    int32_t glyphIndex;
  };

  SdCardFont& font;
  uint16_t generation;
  std::unique_ptr<uint32_t[]> pending;
  std::unique_ptr<CpIdx[]> mappings;
  std::unique_ptr<AdvanceEntry[]> staged;
  uint32_t pendingCount = 0;
  uint8_t styleIdx = 0;
  int missed = 0;
  bool failed = false;

  AdvanceRequest(SdCardFont& owner, uint16_t gen) : font(owner), generation(gen) {}

  bool allocateScratch() {
    pending.reset(new (std::nothrow) uint32_t[ADVANCE_FETCH_BATCH]);
    mappings.reset(new (std::nothrow) CpIdx[ADVANCE_FETCH_BATCH]);
    staged.reset(new (std::nothrow) AdvanceEntry[ADVANCE_FETCH_BATCH]);
    failed = !pending || !mappings || !staged;
    if (failed) {
      LOG_ERR("SDCF", "buildAdvanceTable: failed to allocate request scratch (%u bytes)",
              static_cast<unsigned>(ADVANCE_FETCH_BATCH * (sizeof(uint32_t) + sizeof(CpIdx) + sizeof(AdvanceEntry))));
    }
    return !failed;
  }

  void beginStyle(uint8_t si) {
    styleIdx = si;
    pendingCount = 0;
  }

  void add(uint32_t cp) {
    if (failed || cp == 0 || utf8IsInvisible(cp)) return;
    if (AdvanceEntry* hit = font.findAdvanceEntry(styleIdx, cp)) {
      hit->lastUse = generation;
      return;
    }
    if (font.findUniformAdvance(styleIdx, cp)) return;
    if (!pending && !allocateScratch()) return;
    for (uint32_t i = 0; i < pendingCount; i++) {
      if (pending[i] == cp) return;
    }
    pending[pendingCount++] = cp;
    if (pendingCount == ADVANCE_FETCH_BATCH) flush();
  }

  void addText(const char* text) {
    const auto* p = reinterpret_cast<const unsigned char*>(text);
    while (*p) add(utf8NextCodepoint(&p));
  }

  void flush() {
    if (failed || pendingCount == 0) return;
    const auto& s = font.styles_[styleIdx];
    uint32_t needCount = 0;
    for (uint32_t i = 0; i < pendingCount; i++) {
      const int32_t idx = font.findGlobalGlyphIndex(s, pending[i]);
      if (idx < 0) {
        missed++;
        continue;
      }
      mappings[needCount++] = {pending[i], idx};
    }
    pendingCount = 0;
    if (needCount == 0) return;

    // Sorted by glyph index so the reads walk the glyph table forwards and
    // adjacent glyphs need no seek.
    std::sort(mappings.get(), mappings.get() + needCount,
              [](const CpIdx& a, const CpIdx& b) { return a.glyphIndex < b.glyphIndex; });

    HalFile file;
    if (!Storage.openFileForRead("SDCF", font.filePath_, file)) {
      LOG_ERR("SDCF", "buildAdvanceTable: failed to open .cpfont for style %u", styleIdx);
      return;
    }
    font.stats_.advanceFileOpens++;
    needCount = scanDenseIntervals(file, needCount);
    uint32_t fetched = 0;
    EpdGlyph glyph;
    int32_t lastReadIndex = INT32_MIN;  // forces the first seek (see prewarmStyle)
    for (uint32_t i = 0; i < needCount; i++) {
      const int32_t gIdx = mappings[i].glyphIndex;
      if (gIdx != lastReadIndex + 1) {
        const uint32_t fileOff = s.glyphsFileOffset + static_cast<uint32_t>(gIdx) * sizeof(EpdGlyph);
        if (!file.seekSet(fileOff)) {
          LOG_ERR("SDCF", "buildAdvanceTable: failed to seek to glyph %d (style %u)", gIdx, styleIdx);
          break;
        }
        font.stats_.seekCount++;
      }
      if (file.read(reinterpret_cast<uint8_t*>(&glyph), sizeof(EpdGlyph)) != sizeof(EpdGlyph)) {
        LOG_ERR("SDCF", "buildAdvanceTable: short glyph read (style %u, glyph %d)", styleIdx, gIdx);
        break;
      }
      lastReadIndex = gIdx;
      staged[fetched++] = {mappings[i].codepoint, glyph.advanceX, generation};
    }
    if (fetched == 0) return;

    std::sort(staged.get(), staged.get() + fetched,
              [](const AdvanceEntry& a, const AdvanceEntry& b) { return a.codepoint < b.codepoint; });
    const uint32_t room = font.reserveAdvanceSlots(styleIdx, fetched, generation);
    if (room < fetched) {
      // Only reachable when one request needs more than ADVANCE_REQUEST_LIMIT
      // distinct glyphs (or an allocation failed). Those codepoints measure
      // through the slower per-glyph path; the result is still correct.
      LOG_ERR("SDCF", "Advance table style %u full: %u of %u codepoints not cached", styleIdx, fetched - room, fetched);
    }
    font.insertAdvances(styleIdx, staged.get(), room);
  }

  // Mappings are sorted by glyph index, so misses from one interval are
  // contiguous. A large interval missed densely is scanned whole (one
  // sequential read) and, if uniform, its mappings leave the batch.
  uint32_t scanDenseIntervals(HalFile& file, uint32_t needCount) {
    const auto& s = font.styles_[styleIdx];
    uint32_t kept = 0;
    uint32_t i = 0;
    while (i < needCount) {
      const int32_t intervalIdx = font.findIntervalIndex(s, mappings[i].codepoint);
      uint32_t end = i + 1;
      while (end < needCount && font.findIntervalIndex(s, mappings[end].codepoint) == intervalIdx) end++;
      EpdUnicodeInterval interval{};
      const bool dense = intervalIdx >= 0 && end - i >= UNIFORM_SCAN_MIN_MISSES &&
                         font.intervalAt(s, static_cast<uint32_t>(intervalIdx), &interval) &&
                         interval.last - interval.first + 1 >= UNIFORM_SCAN_MIN_SPAN;
      if (!(dense && scanUniformInterval(file, static_cast<uint32_t>(intervalIdx), interval))) {
        for (uint32_t k = i; k < end; k++) mappings[kept++] = mappings[k];
      }
      i = end;
    }
    return kept;
  }

  bool scanUniformInterval(HalFile& file, uint32_t intervalIdx, const EpdUnicodeInterval& interval) {
    UniformAdvances*& uniform = font.uniform_[styleIdx];
    if (!uniform) {
      uniform = new (std::nothrow) UniformAdvances();
      if (!uniform) return false;
    }
    for (uint8_t t = 0; t < uniform->triedCount; t++) {
      if (uniform->tried[t] == intervalIdx) return false;  // rejected earlier
    }
    if (uniform->triedCount == UniformAdvances::MAX_TRIED) return false;
    uniform->tried[uniform->triedCount++] = intervalIdx;

    const auto& s = font.styles_[styleIdx];
    static constexpr uint32_t CHUNK_GLYPHS = 32;  // one 512-byte SD sector of glyph records
    std::unique_ptr<uint8_t[]> chunk(new (std::nothrow) uint8_t[CHUNK_GLYPHS * sizeof(EpdGlyph)]);
    if (!chunk || !file.seekSet(s.glyphsFileOffset + interval.offset * sizeof(EpdGlyph))) return false;
    font.stats_.uniformScans++;
    font.stats_.seekCount++;

    AdvanceRun found[UNIFORM_MAX_RUNS_PER_INTERVAL] = {};
    uint8_t foundCount = 0;
    const uint32_t span = interval.last - interval.first + 1;
    for (uint32_t done = 0; done < span;) {
      const uint32_t batch = std::min(CHUNK_GLYPHS, span - done);
      const int bytes = static_cast<int>(batch * sizeof(EpdGlyph));
      if (file.read(chunk.get(), batch * sizeof(EpdGlyph)) != bytes) return false;
      for (uint32_t g = 0; g < batch; g++) {
        const uint16_t advance = readU16(chunk.get() + g * sizeof(EpdGlyph) + offsetof(EpdGlyph, advanceX));
        const uint32_t cp = interval.first + done + g;
        if (foundCount > 0 && found[foundCount - 1].advanceX == advance) {
          found[foundCount - 1].last = cp;
          continue;
        }
        if (foundCount == UNIFORM_MAX_RUNS_PER_INTERVAL) return false;  // not uniform enough
        found[foundCount++] = {cp, cp, advance};
      }
      done += batch;
    }
    if (uniform->runCount + foundCount > UniformAdvances::MAX_RUNS) return false;
    for (uint8_t r = 0; r < foundCount; r++) uniform->runs[uniform->runCount++] = found[r];
    LOG_DBG("SDCF", "Uniform advances: U+%04X-U+%04X style %u -> %u run(s)", interval.first, interval.last, styleIdx,
            foundCount);
    return true;
  }
};

int SdCardFont::buildAdvanceTable(const char* utf8Text, uint8_t styleMask) {
  if (loadMode_ == LoadMode::BoundedUI) return checkBoundedText(utf8Text, styleMask);
  if (!loaded_ || !utf8Text) return -1;
  styleMask = resolveStyleMask(styleMask);
  if (styleMask == 0) return 0;
  const unsigned long startMs = millis();
  AdvanceRequest request(*this, beginAdvanceRequest());
  for (uint8_t si = 0; si < MAX_STYLES; si++) {
    if (!(styleMask & (1u << si)) || !styles_[si].present) continue;
    request.beginStyle(si);
    request.addText(utf8Text);
    request.flush();
  }
  stats_.prewarmTotalMs = millis() - startMs;
  return request.failed ? -1 : request.missed;
}

int SdCardFont::buildAdvanceTable(const uint32_t* codepoints, uint32_t count, uint8_t styleMask) {
  if (!loaded_ || (!codepoints && count > 0)) return -1;
  if (loadMode_ == LoadMode::BoundedUI) {
    int missed = 0;
    for (uint32_t i = 0; i < count; i++) {
      for (uint8_t si = 0; si < MAX_STYLES; si++) {
        if ((resolveStyleMask(styleMask) & (1u << si)) && !hasGlyph(codepoints[i], si)) missed++;
      }
    }
    return missed;
  }
  styleMask = resolveStyleMask(styleMask);
  if (styleMask == 0 || count == 0) return 0;
  AdvanceRequest request(*this, beginAdvanceRequest());
  for (uint8_t si = 0; si < MAX_STYLES; si++) {
    if (!(styleMask & (1u << si)) || !styles_[si].present) continue;
    request.beginStyle(si);
    for (uint32_t i = 0; i < count; i++) request.add(codepoints[i]);
    request.flush();
  }
  return request.failed ? -1 : request.missed;
}

int SdCardFont::buildAdvanceTable(const std::vector<std::string>& words, bool includeHyphen, uint8_t styleMask,
                                  const std::vector<EpdFontFamily::Style>* wordStyles) {
  if (loadMode_ == LoadMode::BoundedUI) {
    int missed = 0;
    for (const auto& word : words) {
      const int result = checkBoundedText(word.c_str(), styleMask);
      if (result < 0 || result > INT_MAX - missed) return -1;
      missed += result;
    }
    const int extra = checkBoundedText(includeHyphen ? " -" : " ", styleMask);
    return extra < 0 || extra > INT_MAX - missed ? -1 : missed + extra;
  }
  if (!loaded_) return -1;
  if (wordStyles && wordStyles->size() != words.size()) wordStyles = nullptr;  // defensive: not parallel
  styleMask = resolveStyleMask(styleMask);
  if (styleMask == 0) return 0;
  const unsigned long startMs = millis();
  AdvanceRequest request(*this, beginAdvanceRequest());
  for (uint8_t si = 0; si < MAX_STYLES; si++) {
    if (!(styleMask & (1u << si)) || !styles_[si].present) continue;
    request.beginStyle(si);
    for (size_t w = 0; w < words.size(); w++) {
      if (wordStyles && resolveStyle(static_cast<uint8_t>((*wordStyles)[w]) & (MAX_STYLES - 1)) != si) continue;
      request.addText(words[w].c_str());
    }
    if (words.size() > 1) request.add(' ');
    if (includeHyphen) request.add('-');
    request.flush();
  }
  stats_.prewarmTotalMs = millis() - startMs;
  return request.failed ? -1 : request.missed;
}

// --- Stats ---

void SdCardFont::logStats(const char* label) {
  LOG_DBG("SDCF", "[%s] total=%ums sd_read=%ums seeks=%u glyphs=%u bitmap=%u bytes", label, stats_.prewarmTotalMs,
          stats_.sdReadTimeMs, stats_.seekCount, stats_.uniqueGlyphs, stats_.bitmapBytes);
}

void SdCardFont::resetStats() { stats_ = Stats{}; }

// --- Public accessors ---

EpdFont* SdCardFont::getEpdFont(uint8_t style) {
  style &= (MAX_STYLES - 1);
  if (!styles_[style].present) return nullptr;
  return &styles_[style].epdFont;
}

bool SdCardFont::hasStyle(uint8_t style) const { return styles_[style & (MAX_STYLES - 1)].present; }

uint8_t SdCardFont::resolveStyle(uint8_t style) const {
  static const uint8_t kFallbacks[MAX_STYLES][MAX_STYLES] = {
      // REGULAR: REGULAR -> BOLD -> ITALIC -> BOLD_ITALIC
      {EpdFontFamily::REGULAR, EpdFontFamily::BOLD, EpdFontFamily::ITALIC, EpdFontFamily::BOLD_ITALIC},
      // BOLD: BOLD -> REGULAR -> BOLD_ITALIC -> ITALIC
      {EpdFontFamily::BOLD, EpdFontFamily::REGULAR, EpdFontFamily::BOLD_ITALIC, EpdFontFamily::ITALIC},
      // ITALIC: ITALIC -> REGULAR -> BOLD_ITALIC -> BOLD
      {EpdFontFamily::ITALIC, EpdFontFamily::REGULAR, EpdFontFamily::BOLD_ITALIC, EpdFontFamily::BOLD},
      // BOLD_ITALIC: BOLD_ITALIC -> BOLD -> ITALIC -> REGULAR
      {EpdFontFamily::BOLD_ITALIC, EpdFontFamily::BOLD, EpdFontFamily::ITALIC, EpdFontFamily::REGULAR},
  };

  const uint8_t styleBits = style & (MAX_STYLES - 1);
  for (uint8_t candidate : kFallbacks[styleBits]) {
    if (styles_[candidate].present) return candidate;
  }
  return EpdFontFamily::REGULAR;
}

uint8_t SdCardFont::resolveStyleMask(uint8_t styleMask) const {
  uint8_t resolvedMask = 0;
  for (uint8_t si = 0; si < MAX_STYLES; si++) {
    if (styleMask & (1 << si)) {
      resolvedMask |= static_cast<uint8_t>(1u << resolveStyle(si));
    }
  }
  return resolvedMask;
}

// --- On-demand glyph loading (overflow buffer) ---

namespace {
uint8_t readKernClass(HalFile& file, uint32_t offset, uint16_t count, uint32_t cp, bool& failed) {
  if (cp > UINT16_MAX) return 0;
  uint16_t lo = 0, hi = count;
  while (lo < hi) {
    const uint16_t mid = lo + (hi - lo) / 2;
    uint8_t entry[3];
    if (!file.seekSet(offset + mid * sizeof(EpdKernClassEntry)) || file.read(entry, sizeof(entry)) != sizeof(entry)) {
      failed = true;
      return 0;
    }
    const uint16_t found = readU16(entry);
    if (found == cp) return entry[2];
    if (found < cp)
      lo = mid + 1;
    else
      hi = mid;
  }
  return 0;
}
}  // namespace

int8_t SdCardFont::lookupBoundedKerning(void* context, uint32_t left, uint32_t right) {
  const auto& ctx = *static_cast<OverflowContext*>(context);
  const auto& self = *ctx.self;
  const auto& s = self.styles_[ctx.styleIdx];
  if (!self.loaded_ || !s.header.kernLeftEntryCount || !s.header.kernRightEntryCount) return 0;
  if (self.progress_) self.progress_();
  HalFile file;
  if (!Storage.openFileForRead("SDCF", self.filePath_, file)) {
    self.boundedReadFailed_ = true;
    return 0;
  }
  const uint8_t lc =
      readKernClass(file, s.kernLeftFileOffset, s.header.kernLeftEntryCount, left, self.boundedReadFailed_);
  const uint8_t rc =
      readKernClass(file, s.kernRightFileOffset, s.header.kernRightEntryCount, right, self.boundedReadFailed_);
  if (lc > s.header.kernLeftClassCount || rc > s.header.kernRightClassCount) self.boundedReadFailed_ = true;
  if (!lc || !rc || self.boundedReadFailed_) return 0;
  int8_t adjustment = 0;
  const size_t position = s.kernMatrixFileOffset + (lc - 1) * s.header.kernRightClassCount + rc - 1;
  if (!file.seekSet(position) || file.read(&adjustment, 1) != 1) {
    self.boundedReadFailed_ = true;
    return 0;
  }
  return adjustment;
}

uint32_t SdCardFont::lookupBoundedLigature(void* context, uint32_t left, uint32_t right) {
  const auto& ctx = *static_cast<OverflowContext*>(context);
  const auto& self = *ctx.self;
  const auto& s = self.styles_[ctx.styleIdx];
  if (!self.loaded_ || !s.header.ligaturePairCount || left > UINT16_MAX || right > UINT16_MAX) return 0;
  if (self.progress_) self.progress_();
  HalFile file;
  if (!Storage.openFileForRead("SDCF", self.filePath_, file)) {
    self.boundedReadFailed_ = true;
    return 0;
  }
  uint16_t lo = 0, hi = s.header.ligaturePairCount;
  const uint32_t key = (left << 16) | right;
  while (lo < hi) {
    const uint16_t mid = lo + (hi - lo) / 2;
    uint8_t entry[8];
    if (!file.seekSet(s.ligatureFileOffset + mid * sizeof(EpdLigaturePair)) ||
        file.read(entry, sizeof(entry)) != sizeof(entry)) {
      self.boundedReadFailed_ = true;
      return 0;
    }
    const uint32_t pair = readU32(entry);
    if (pair == key) {
      const uint32_t cp = readU32(entry + 4);
      if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        self.boundedReadFailed_ = true;
        return 0;
      }
      return cp;
    }
    if (pair < key)
      lo = mid + 1;
    else
      hi = mid;
  }
  return 0;
}

int SdCardFont::checkBoundedText(const char* text, uint8_t styleMask) {
  if (!loaded_ || !text) return -1;
  styleMask = resolveStyleMask(styleMask);
  int missed = 0;
  const auto* p = reinterpret_cast<const unsigned char*>(text);
  while (*p) {
    const uint32_t cp = utf8NextCodepoint(&p);
    if (cp == '\n' || cp == '\r' || cp == '\t') continue;
    for (uint8_t style = 0; style < MAX_STYLES; ++style) {
      if ((styleMask & (1U << style)) && !onGlyphMiss(&overflowCtx_[style], cp)) {
        if (missed == INT_MAX) return -1;
        ++missed;
      }
    }
  }
  return missed;
}

const EpdGlyph* SdCardFont::onGlyphMiss(void* ctx, uint32_t codepoint) {
  auto* oc = static_cast<OverflowContext*>(ctx);
  auto* self = oc->self;
  uint8_t styleIdx = oc->styleIdx;

  if (!self->loaded_ || styleIdx >= MAX_STYLES || !self->styles_[styleIdx].present) return nullptr;
  const auto& s = self->styles_[styleIdx];
  if (self->loadMode_ != LoadMode::BoundedUI && !s.fullIntervals && !s.bmpIntervals) return nullptr;

  // Check overflow cache first (matching both codepoint and style)
  for (uint32_t i = 0; i < self->overflowCount_; i++) {
    if (self->overflow_[i].codepoint == codepoint && self->overflow_[i].styleIdx == styleIdx) {
      return &self->overflow_[i].glyph;
    }
  }

  // Look up global glyph index via full intervals. Cached mode answers from
  // RAM; BoundedUI reads SD, so its recent misses are remembered.
  const bool bounded = self->loadMode_ == LoadMode::BoundedUI;
  if (bounded && self->recentlyMissed(codepoint, styleIdx)) return nullptr;
  int32_t globalIdx = self->findGlobalGlyphIndex(s, codepoint);
  if (globalIdx < 0) {
    if (bounded && !self->boundedReadFailed_) self->rememberMiss(codepoint, styleIdx);
    return nullptr;
  }

  // Pick overflow slot (ring buffer). Read into temporaries first so the
  // existing slot stays valid if SD I/O fails. Bookkeeping (count/next)
  // is deferred until after all I/O succeeds to avoid inconsistent state.
  uint32_t slot = self->overflowNext_;
  bool wasAtCapacity = (self->overflowCount_ == OVERFLOW_CAPACITY);

  // Read glyph metadata into temporary
  HalFile file;
  if (!Storage.openFileForRead("SDCF", self->filePath_, file)) {
    if (self->loadMode_ == LoadMode::BoundedUI) self->boundedReadFailed_ = true;
    LOG_ERR("SDCF", "Overflow: failed to open .cpfont");
    return nullptr;
  }

  EpdGlyph tempGlyph = {};
  uint32_t glyphFileOff = s.glyphsFileOffset + static_cast<uint32_t>(globalIdx) * sizeof(EpdGlyph);
  if (!file.seekSet(glyphFileOff)) {
    if (self->loadMode_ == LoadMode::BoundedUI) self->boundedReadFailed_ = true;
    LOG_ERR("SDCF", "Overflow: failed to seek to glyph for U+%04X style %u", codepoint, styleIdx);
    file.close();
    return nullptr;
  }
  if (file.read(reinterpret_cast<uint8_t*>(&tempGlyph), sizeof(EpdGlyph)) != sizeof(EpdGlyph)) {
    if (self->loadMode_ == LoadMode::BoundedUI) self->boundedReadFailed_ = true;
    LOG_ERR("SDCF", "Overflow: failed to read glyph metadata for U+%04X style %u", codepoint, styleIdx);
    return nullptr;
  }
  if (self->loadMode_ == LoadMode::BoundedUI &&
      (tempGlyph.width > 64 || tempGlyph.height > 64 || tempGlyph.dataLength > UI_GLYPH_BYTES ||
       tempGlyph.dataLength !=
           (static_cast<unsigned>(tempGlyph.width) * tempGlyph.height * (s.header.is2Bit ? 2 : 1) + 7) / 8 ||
       static_cast<uint64_t>(s.bitmapFileOffset) + tempGlyph.dataOffset + tempGlyph.dataLength >
           self->loadedFileSize_)) {
    LOG_ERR("SDCF", "Glyph exceeds bounded UI budget or font file");
    self->boundedReadFailed_ = true;
    return nullptr;
  }

  // Read bitmap data into temporary (if any)
  std::unique_ptr<uint8_t[]> pendingBitmap;
  uint8_t* tempBitmap = nullptr;
  if (tempGlyph.dataLength > 0) {
    // Existing ring owns this allocation after successful read. BoundedUI
    // retains <=8*256B, with one <=256B replacement transient; no page cache.
    pendingBitmap = makeUniqueNoThrow<uint8_t[]>(tempGlyph.dataLength);
    tempBitmap = pendingBitmap.get();
    if (!tempBitmap) {
      if (self->loadMode_ == LoadMode::BoundedUI) self->boundedReadFailed_ = true;
      LOG_ERR("SDCF", "Overflow: failed to allocate %u bytes for U+%04X bitmap", tempGlyph.dataLength, codepoint);
      return nullptr;
    }
    if (!file.seekSet(s.bitmapFileOffset + tempGlyph.dataOffset)) {
      if (self->loadMode_ == LoadMode::BoundedUI) self->boundedReadFailed_ = true;
      LOG_ERR("SDCF", "Overflow: failed to seek to bitmap for U+%04X", codepoint);
      file.close();
      return nullptr;
    }
    if (file.read(tempBitmap, tempGlyph.dataLength) != static_cast<int>(tempGlyph.dataLength)) {
      if (self->loadMode_ == LoadMode::BoundedUI) self->boundedReadFailed_ = true;
      LOG_ERR("SDCF", "Overflow: failed to read bitmap for U+%04X", codepoint);
      return nullptr;
    }
  }

  // All reads succeeded — commit to slot and advance ring buffer
  if (wasAtCapacity) {
    delete[] self->overflow_[slot].bitmap;
  } else {
    self->overflowCount_++;
  }
  self->overflowNext_ = (slot + 1) % OVERFLOW_CAPACITY;
  self->overflow_[slot].glyph = tempGlyph;
  self->overflow_[slot].bitmap = pendingBitmap.release();
  self->overflow_[slot].codepoint = codepoint;
  self->overflow_[slot].styleIdx = styleIdx;

  LOG_DBG("SDCF", "Overflow: loaded U+%04X style %u on demand (slot %u/%u)", codepoint, styleIdx, slot,
          OVERFLOW_CAPACITY);

  return &self->overflow_[slot].glyph;
}

bool SdCardFont::recentlyMissed(uint32_t codepoint, uint8_t styleIdx) const {
  const uint32_t key = (codepoint << 2) | (styleIdx & (MAX_STYLES - 1));
  for (uint8_t i = 0; i < missCacheCount_; i++) {
    if (missCache_[i] == key) return true;
  }
  return false;
}

void SdCardFont::rememberMiss(uint32_t codepoint, uint8_t styleIdx) const {
  missCache_[missCacheNext_] = (codepoint << 2) | (styleIdx & (MAX_STYLES - 1));
  missCacheNext_ = static_cast<uint8_t>((missCacheNext_ + 1) % MISS_CACHE_SIZE);
  if (missCacheCount_ < MISS_CACHE_SIZE) missCacheCount_++;
}

bool SdCardFont::isOverflowGlyph(const EpdGlyph* glyph) const {
  for (uint32_t i = 0; i < overflowCount_; i++) {
    if (&overflow_[i].glyph == glyph) return true;
  }
  return false;
}

const uint8_t* SdCardFont::getOverflowBitmap(const EpdGlyph* glyph) const {
  for (uint32_t i = 0; i < overflowCount_; i++) {
    if (&overflow_[i].glyph == glyph) {
      return overflow_[i].bitmap;
    }
  }
  return nullptr;
}

SdCardFont* SdCardFont::fromMissCtx(void* ctx) { return static_cast<OverflowContext*>(ctx)->self; }
