#pragma once

#include <SdCardFontManager.h>
#include <SdCardFontRegistry.h>

#include <atomic>

class GfxRenderer;

/// Facade that owns the SD card font registry, manager, and resolver logic.
/// Hides implementation details behind a single begin() + ensureLoaded() API.
class SdCardFontSystem {
 public:
  SdCardFontSystem() = default;
  SdCardFontSystem(const SdCardFontSystem&) = delete;
  SdCardFontSystem& operator=(const SdCardFontSystem&) = delete;
  /// Discover SD card fonts and load user's saved selection. Call once during setup.
  void begin(GfxRenderer& renderer);

  /// Ensure the correct SD font family is loaded for the current settings.
  /// Call before entering the reader or after settings change.
  /// Also re-discovers if the registry has been marked dirty (e.g. by web upload).
  void ensureLoaded(GfxRenderer& renderer);

  /// Unload the resident SD font family without changing the saved selection.
  /// Memory-tight activities that need TLS buffers can call this before opening
  /// network connections; the reader will reload the saved family on demand.
  void releaseLoaded(GfxRenderer& renderer);

  /// Load a specific SD font family at the UI point size without changing the
  /// saved reader selection. Returns the loaded font ID, or 0 if unavailable.
  int ensureUiFamilyLoaded(GfxRenderer& renderer, const char* familyName,
                           SdCardFont::LoadMode mode = SdCardFont::LoadMode::Cached, void (*progress)() = nullptr);
  bool boundedUiFontReady(int fontId) const {
    return fontId != 0 && fontId == currentLoadedFontId() && manager_.loadMode() == SdCardFont::LoadMode::BoundedUI &&
           !manager_.boundedReadFailed();
  }

  /// Load Pocket Daily's broad reading family without changing the saved
  /// reader selection. All supported book languages use the same flattened
  /// family, so callers never need to ask the user to choose a script font.
  /// Falls back to the saved reader font when the automatic pack is absent.
  int ensureAutomaticReaderFontLoaded(GfxRenderer& renderer);

  /// Font ID of the currently-loaded SD family (0 if none). Lets callers that
  /// want to avoid single-slot unload/reload churn test the resident font
  /// against their text before requesting a swap via ensureUiFamilyLoaded().
  int currentLoadedFontId() const;

  /// Resolve an SD card font ID from family name + fontSize enum.
  /// Returns 0 if not found. Used by CrossPointSettings::getReaderFontId().
  int resolveFontId(const char* familyName, uint8_t fontSizeEnum) const;

  /// Access the registry (e.g. for settings UI to enumerate available fonts).
  const SdCardFontRegistry& registry() const { return registry_; }

  /// Non-const access to the registry (for FontInstaller).
  SdCardFontRegistry& registry() { return registry_; }

  /// Mark the registry as needing re-discovery.
  /// Thread-safe: can be called from the web server task.
  void markRegistryDirty() { registryDirty_.store(true, std::memory_order_release); }

  /// If the registry is dirty, re-scan the SD card now and clear the flag.
  /// Used by the web UI so uploaded/deleted fonts appear in the list
  /// without waiting for the reader activity to run ensureLoaded().
  void refreshIfDirty() {
    if (registryDirty_.exchange(false, std::memory_order_acquire)) {
      rediscover();
    }
  }

  /// The glyph-fallback font (SdCardFontRegistry::FALLBACK_FAMILY) if it is
  /// currently loaded; never loads it. Exposed for diagnostics and tests.
  const SdCardFont* loadedFallbackFont() const { return fallback_; }

 private:
  SdCardFontRegistry registry_;
  SdCardFontManager manager_;
  std::atomic<bool> registryDirty_{false};

  // Glyph fallback (symbols/emoji). Loaded lazily in Cached mode the first time
  // a reader or Cached UI font lacks a visible glyph, and only while enabled:
  // reader and Cached UI paths enable it; releaseLoaded() and BoundedUI loads
  // (network-mode live views) disable and free it. A missing or unloadable
  // file is remembered so repeated misses cost no SD access.
  GfxRenderer* renderer_ = nullptr;  // set by begin(); receives the fallback provider and layout key
  SdCardFont* fallback_ = nullptr;   // owned
  uint8_t fallbackPointSize_ = 0;
  bool fallbackEnabled_ = false;
  bool fallbackUnavailable_ = false;
  static SdCardFont* provideFallback(void* context, bool load);
  SdCardFont* fallbackFont(bool load);
  void setFallbackEnabled(bool enabled);
  void dropFallback();
  void rediscover();
  void publishFallbackLayoutKey();
};

// Global SD card font system instance (defined in main.cpp).
extern SdCardFontSystem sdFontSystem;
