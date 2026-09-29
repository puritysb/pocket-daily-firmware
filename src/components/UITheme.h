#pragma once

#include <EpdFontFamily.h>

#include <functional>
#include <memory>

#include "CrossPointSettings.h"
#include "components/themes/BaseTheme.h"
#include "pocket_daily/live_studio/UiPack.h"

class CoverGridHomeUi;

class UITheme {
  // Static instance
  static UITheme instance;

 public:
  enum class TextVerticalAlignment { TOP, CENTER, BOTTOM };

  UITheme();
  static UITheme& getInstance() { return instance; }

  const ThemeMetrics& getMetrics() const;
  const BaseTheme& getTheme() const { return currentTheme ? *currentTheme : fallbackTheme; }
  Rect getScreenSafeArea(const GfxRenderer& renderer, bool hasFrontButtonHints = false,
                         bool hasSideButtonHints = false);
  static void drawCenteredText(const GfxRenderer& renderer, Rect screen, int fontId, int y, const char* text,
                               bool black = true, EpdFontFamily::Style style = EpdFontFamily::REGULAR);
  // Wraps only overflowing text, then aligns the complete line block within bounds.
  static void drawCenteredWrappedText(const GfxRenderer& renderer, Rect bounds, int fontId, const char* text,
                                      int maxLines, bool black = true,
                                      EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                                      TextVerticalAlignment verticalAlignment = TextVerticalAlignment::CENTER);
  static bool supportsCoverGrid();
  static bool hasCoverGridHome();
  static void drawCoverGridHome(CoverGridHomeUi& home);
  void reload();
  void setTheme(CrossPointSettings::UI_THEME type);
  // Live Studio LS-3: apply a validated .uipack's theme overrides on top of
  // the current theme. An empty list reverts to the theme's own metrics.
  // Takes ownership of malloc-backed, already validated overrides; no allocation.
  // Caller holds RenderLock when the render task is running.
  void adoptPackMetrics(PocketDaily::LiveStudio::ThemeOverride* overrides, size_t count);
  bool packActive() const { return packOverrides != nullptr; }
  static int getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasTabBar, bool hasButtonHints,
                                     bool hasSubtitle, int extraReservedHeight = 0);
  static std::string getCoverThumbPath(std::string coverBmpPath, int coverHeight);
  static UIIcon getFileIcon(const std::string& filename);
  static int getStatusBarHeight();
  static int getProgressBarHeight();

 private:
  void reapplyPackAfterThemeChange();

  BaseTheme fallbackTheme;
  const ThemeMetrics* currentMetrics = &BaseMetrics::values;
  const ThemeMetrics* baseMetrics = nullptr;
  std::unique_ptr<BaseTheme> currentTheme;
  // Pack state lives on the heap only while a pack is active - the File
  // Transfer baseline must stay at its no-pack level (radio buffer budget).
  ThemeMetrics packedMetrics{};
  PocketDaily::LiveStudio::ThemeOverride* packOverrides = nullptr;
  size_t packOverrideCount = 0;
  mutable ThemeMetrics adjustedMetrics;
  mutable bool metricsValid = false;
  mutable bool metricsForTouch = false;
};

// Helper macro to access current theme
#define GUI UITheme::getInstance().getTheme()
