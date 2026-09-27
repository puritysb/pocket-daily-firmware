#include "SdCardFontSystem.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include "CrossPointSettings.h"

namespace {

static uint8_t fontSizeEnumFromSettings() {
  uint8_t e = SETTINGS.fontSize;
  if (e >= CrossPointSettings::FONT_SIZE_COUNT) e = 1;  // default to MEDIUM
  return e;
}

}  // namespace

void SdCardFontSystem::begin(GfxRenderer& renderer) {
  registry_.discover();
  renderer_ = &renderer;
  renderer.setFallbackFontProvider(&SdCardFontSystem::provideFallback, this);
  publishFallbackLayoutKey();

  // Register this system as the SD font ID resolver in settings.
  // Uses a static trampoline since CrossPointSettings stores a plain function pointer.
  SETTINGS.sdFontIdResolver = [](void* ctx, const char* familyName, uint8_t fontSizeEnum) -> int {
    return static_cast<SdCardFontSystem*>(ctx)->resolveFontId(familyName, fontSizeEnum);
  };
  SETTINGS.sdFontResolverCtx = this;

  // If user has a saved SD font selection, load it
  if (SETTINGS.sdFontFamilyName[0] != '\0') {
    const auto* family = registry_.findFamily(SETTINGS.sdFontFamilyName);
    if (family) {
      if (manager_.loadFamily(*family, renderer, fontSizeEnumFromSettings())) {
        LOG_DBG("SDFS", "Loaded SD card font family: %s", SETTINGS.sdFontFamilyName);
      } else {
        LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", SETTINGS.sdFontFamilyName);
        SETTINGS.sdFontFamilyName[0] = '\0';
        SETTINGS.saveToFile();
      }
    } else {
      LOG_DBG("SDFS", "SD font family not found on card: %s (clearing)", SETTINGS.sdFontFamilyName);
      SETTINGS.sdFontFamilyName[0] = '\0';
      SETTINGS.saveToFile();
    }
  }

  LOG_DBG("SDFS", "SD font system ready (%d families discovered)", registry_.getFamilyCount());
}

void SdCardFontSystem::ensureLoaded(GfxRenderer& renderer) {
  // If the web server (or another task) installed/deleted fonts, re-discover.
  // Track whether we just re-discovered so we can force a reload below even
  // when the wanted family/size still maps to the same point size — the file
  // contents on disk may have changed (e.g. user re-uploaded a new build).
  const bool registryWasDirty = registryDirty_.exchange(false, std::memory_order_acquire);
  if (registryWasDirty) {
    LOG_DBG("SDFS", "Registry dirty — re-discovering fonts");
    rediscover();
  }
  setFallbackEnabled(true);

  const char* wantedFamily = SETTINGS.sdFontFamilyName;
  const std::string& currentFamily = manager_.currentFamilyName();
  const uint8_t sizeEnum = fontSizeEnumFromSettings();

  if (wantedFamily[0] == '\0') {
    if (!currentFamily.empty()) {
      manager_.unloadAll(renderer);
    }
    return;
  }

  // Reload if family changed OR if the user-selected size maps to a
  // different file than what's currently loaded OR if the registry was
  // just rediscovered (file may have been replaced on disk).
  bool familyMatches = (currentFamily == wantedFamily);
  if (familyMatches) {
    const auto* family = registry_.findFamily(wantedFamily);
    if (!family) {
      LOG_DBG("SDFS", "SD font family disappeared: %s (clearing)", wantedFamily);
      manager_.unloadAll(renderer);
      SETTINGS.sdFontFamilyName[0] = '\0';
      SETTINGS.saveToFile();
      return;
    }
    const auto* selected = family->findClosestReaderSize(sizeEnum);
    const uint8_t wantedPt = selected ? selected->pointSize : 0;
    if (!registryWasDirty && wantedPt == manager_.currentPointSize() &&
        manager_.loadMode() == SdCardFont::LoadMode::Cached)
      return;
    LOG_DBG("SDFS", "Reloading %s: size %u -> %u (enum %u)%s", wantedFamily, manager_.currentPointSize(), wantedPt,
            sizeEnum, registryWasDirty ? " [registry dirty]" : "");
  }

  if (!currentFamily.empty()) {
    manager_.unloadAll(renderer);
  }

  const auto* family = registry_.findFamily(wantedFamily);
  if (family) {
    if (manager_.loadFamily(*family, renderer, sizeEnum)) {
      LOG_DBG("SDFS", "Loaded SD font family: %s", wantedFamily);
    } else {
      LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", wantedFamily);
      SETTINGS.sdFontFamilyName[0] = '\0';
      SETTINGS.saveToFile();
    }
  } else {
    LOG_DBG("SDFS", "SD font family not found: %s (clearing)", wantedFamily);
    SETTINGS.sdFontFamilyName[0] = '\0';
    SETTINGS.saveToFile();
  }
}

void SdCardFontSystem::releaseLoaded(GfxRenderer& renderer) {
  setFallbackEnabled(false);
  if (!manager_.currentFamilyName().empty()) {
    LOG_DBG("SDFS", "Releasing loaded SD font family: %s", manager_.currentFamilyName().c_str());
    manager_.unloadAll(renderer);
  }
}

int SdCardFontSystem::ensureUiFamilyLoaded(GfxRenderer& renderer, const char* familyName, SdCardFont::LoadMode mode,
                                           void (*progress)()) {
  if (!familyName || !familyName[0]) return 0;

  refreshIfDirty();
  // Live network-mode views use BoundedUI and must not pull in a second font.
  setFallbackEnabled(mode == SdCardFont::LoadMode::Cached);

  static constexpr uint8_t uiFontSizeEnum = CrossPointSettings::SMALL;
  const auto* family = registry_.findFamily(familyName);
  if (!family) return 0;

  const auto* selected = family->findClosestReaderSize(uiFontSizeEnum);
  const uint8_t wantedPt = selected ? selected->pointSize : 0;
  if (manager_.currentFamilyName() == familyName && manager_.currentPointSize() == wantedPt &&
      manager_.loadMode() == mode && (mode != SdCardFont::LoadMode::BoundedUI || !manager_.boundedReadFailed())) {
    return manager_.getFontId(familyName);
  }

  if (!manager_.currentFamilyName().empty()) {
    manager_.unloadAll(renderer);
  }

  if (!manager_.loadFamily(*family, renderer, uiFontSizeEnum, mode, progress)) {
    LOG_ERR("SDFS", "Failed to load UI SD font family: %s", familyName);
    return 0;
  }
  return manager_.getFontId(familyName);
}

int SdCardFontSystem::ensureAutomaticReaderFontLoaded(GfxRenderer& renderer) {
  static constexpr char automaticFamily[] = "PocketSansWorld";

  refreshIfDirty();
  setFallbackEnabled(true);
  const auto* family = registry_.findFamily(automaticFamily);
  if (!family) {
    ensureLoaded(renderer);
    return SETTINGS.getReaderFontId();
  }

  const uint8_t sizeEnum = fontSizeEnumFromSettings();
  const auto* selected = family->findClosestReaderSize(sizeEnum);
  const uint8_t wantedPt = selected ? selected->pointSize : 0;
  if (manager_.currentFamilyName() == automaticFamily && manager_.currentPointSize() == wantedPt &&
      manager_.loadMode() == SdCardFont::LoadMode::Cached) {
    return manager_.getFontId(automaticFamily);
  }

  if (!manager_.currentFamilyName().empty()) {
    manager_.unloadAll(renderer);
  }
  if (!manager_.loadFamily(*family, renderer, sizeEnum)) {
    LOG_ERR("SDFS", "Failed to load automatic reader font: %s", automaticFamily);
    ensureLoaded(renderer);
    return SETTINGS.getReaderFontId();
  }

  LOG_DBG("SDFS", "Loaded automatic reader font: %s", automaticFamily);
  return manager_.getFontId(automaticFamily);
}

int SdCardFontSystem::currentLoadedFontId() const {
  const std::string& name = manager_.currentFamilyName();
  if (name.empty()) return 0;
  return manager_.getFontId(name);
}

int SdCardFontSystem::resolveFontId(const char* familyName, uint8_t /*fontSizeEnum*/) const {
  // The manager loads exactly one size (closest to SETTINGS.fontSize), so the
  // enum is implicit — always return the single loaded font ID for this family.
  // ensureLoaded() must have been called with the current settings before this.
  return manager_.getFontId(familyName);
}

// --- Glyph fallback font ---

SdCardFont* SdCardFontSystem::provideFallback(void* context, const bool load) {
  return static_cast<SdCardFontSystem*>(context)->fallbackFont(load);
}

SdCardFont* SdCardFontSystem::fallbackFont(const bool load) {
  if (!fallbackEnabled_) return nullptr;
  if (!load) return fallback_;
  if (fallbackUnavailable_) return fallback_;

  const auto* family = registry_.fallbackFamily();
  if (!family) {
    fallbackUnavailable_ = true;  // not installed: remember, no SD access per miss
    return nullptr;
  }
  // Match the resident reader font's pixel size when one is loaded (packs may
  // ship a single size); otherwise follow the reader size setting.
  const SdCardFontFileInfo* selected = nullptr;
  const uint8_t primaryPt = manager_.currentPointSize();
  if (primaryPt != 0) {
    uint8_t bestDelta = 255;
    for (const auto& file : family->files) {
      const uint8_t delta = file.pointSize > primaryPt ? file.pointSize - primaryPt : primaryPt - file.pointSize;
      if (!selected || delta < bestDelta) {
        selected = &file;
        bestDelta = delta;
      }
    }
  } else {
    selected = family->findClosestReaderSize(fontSizeEnumFromSettings());
  }
  if (!selected) {
    fallbackUnavailable_ = true;
    return nullptr;
  }
  if (fallback_ && fallbackPointSize_ == selected->pointSize) return fallback_;
  dropFallback();

  auto* font = new (std::nothrow) SdCardFont();
  if (!font) {
    LOG_ERR("SDFS", "Glyph fallback: out of memory");
    fallbackUnavailable_ = true;  // retried on the next enable, not per glyph
    return nullptr;
  }
  // Cached mode keeps the (small) interval table resident, so coverage checks
  // and negative lookups never touch the SD card.
  if (!font->load(selected->path.c_str(), SdCardFont::LoadMode::Cached)) {
    LOG_ERR("SDFS", "Glyph fallback: failed to load %s", selected->path.c_str());
    delete font;
    fallbackUnavailable_ = true;
    return nullptr;
  }
  fallback_ = font;
  fallbackPointSize_ = selected->pointSize;
  LOG_DBG("SDFS", "Glyph fallback loaded: %s", selected->path.c_str());
  return fallback_;
}

void SdCardFontSystem::setFallbackEnabled(const bool enabled) {
  if (enabled == fallbackEnabled_) return;
  fallbackEnabled_ = enabled;
  fallbackUnavailable_ = false;
  if (!enabled) dropFallback();
}

void SdCardFontSystem::dropFallback() {
  delete fallback_;
  fallback_ = nullptr;
  fallbackPointSize_ = 0;
}

void SdCardFontSystem::rediscover() {
  dropFallback();
  fallbackUnavailable_ = false;
  registry_.discover();
  publishFallbackLayoutKey();
}

// Section and TXT page caches mix this key into their font field: glyphs the
// fallback supplies change measured widths, so installing, removing or
// replacing the fallback family re-lays out cached books. FNV-1a over the
// discovered files' paths and point sizes (a rebuilt file under the same name
// is not detected); 0 when no fallback family is installed.
void SdCardFontSystem::publishFallbackLayoutKey() {
  if (!renderer_) return;
  const auto* family = registry_.fallbackFamily();
  uint32_t key = 0;
  if (family) {
    key = 2166136261u;
    for (const auto& file : family->files) {
      for (const char c : file.path) {
        key ^= static_cast<uint8_t>(c);
        key *= 16777619u;
      }
      key ^= file.pointSize;
      key *= 16777619u;
    }
    if (key == 0) key = 1;
  }
  renderer_->setGlyphLayoutKey(key);
}
