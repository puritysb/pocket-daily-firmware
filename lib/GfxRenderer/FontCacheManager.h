#pragma once

#include <EpdFontFamily.h>

#include <cstdint>
#include <map>
#include <string>

class FontDecompressor;
class GfxRenderer;
class SdCardFont;

class FontCacheManager {
 public:
  FontCacheManager(const std::map<int, EpdFontFamily>& fontMap, const std::map<int, SdCardFont*>& sdCardFonts);

  void setFontDecompressor(FontDecompressor* d);
  // Set by GfxRenderer::setFontCacheManager(); gives page prewarm access to the
  // renderer's glyph-fallback font.
  void attachRenderer(const GfxRenderer* renderer) { renderer_ = renderer; }

  void clearCache();

  // Cross-page glyph reuse (SdCardFont::retainPageGlyphs). At the end of each page render
  // the policy is asked whether the page's SD glyph set (`bytes`) may stay resident until
  // the next page's prewarm, which then copies the shared glyphs instead of reading them
  // again. No policy: the set is freed, as before. dropRetainedGlyphs() frees a retained
  // set early (before anything that needs the heap, such as a section build).
  using RetainPolicyFn = bool (*)(void* context, uint32_t bytes);
  void setRetainPolicy(RetainPolicyFn policy, void* context) {
    retainPolicy_ = policy;
    retainContext_ = context;
  }
  void dropRetainedGlyphs();
  // `extraText` (optional) loads with the page's glyphs but stays out of the SD font's
  // per-page kerning classes, so it renders exactly as if loaded on demand.
  void prewarmCache(int fontId, const char* utf8Text, uint8_t styleMask = 0x0F, const char* extraText = nullptr);
  void logStats(const char* label = "render");
  void resetStats();

  // Scan-mode API: called by GfxRenderer::drawText() during scan pass
  bool isScanning() const;
  void recordText(const char* text, int fontId, EpdFontFamily::Style style);
  // UI text drawn after the page with fontId (e.g. the status-bar chapter title): its
  // glyphs join the page's resident set so drawing it needs no per-glyph SD loads.
  // Call during the scan pass; kerning stays as for on-demand glyphs.
  void recordExtraText(const char* text, int fontId);

  // True between PrewarmScope::endScanAndPrewarm() and the scope's end. The page's glyph set
  // is resident for every remaining pass of that page (BW, then each grayscale strip), so a
  // one-off prewarm (e.g. a CJK status-bar title) must not replace it: see
  // GfxRenderer::prewarmSdCardFont().
  bool isPageGlyphSetPinned() const { return pageGlyphSetPinned_; }

  // The FontDecompressor pointer, needed by GfxRenderer::getGlyphBitmap()
  FontDecompressor* getDecompressor() const { return fontDecompressor_; }

  // RAII scope for two-pass prewarm pattern
  class PrewarmScope {
   public:
    explicit PrewarmScope(FontCacheManager& manager);
    ~PrewarmScope();
    void endScanAndPrewarm();
    PrewarmScope(PrewarmScope&& other) noexcept;
    PrewarmScope& operator=(PrewarmScope&&) = delete;
    PrewarmScope(const PrewarmScope&) = delete;
    PrewarmScope& operator=(const PrewarmScope&) = delete;

   private:
    FontCacheManager* manager_;
    bool active_ = true;
  };
  PrewarmScope createPrewarmScope();

 private:
  const std::map<int, EpdFontFamily>& fontMap_;
  const std::map<int, SdCardFont*>& sdCardFonts_;
  FontDecompressor* fontDecompressor_ = nullptr;
  const GfxRenderer* renderer_ = nullptr;

  enum class ScanMode : uint8_t { None, Scanning };
  ScanMode scanMode_ = ScanMode::None;
  bool pageGlyphSetPinned_ = false;
  RetainPolicyFn retainPolicy_ = nullptr;
  void* retainContext_ = nullptr;
  // Keeps the page's SD glyph sets for the next prewarm when the policy allows.
  void retainOrClear();
  struct ScanBucket {
    std::string text;
    std::string extraText;
    uint32_t styleCounts[4] = {};
  };
  std::map<int, ScanBucket> scanBuckets_;
};
