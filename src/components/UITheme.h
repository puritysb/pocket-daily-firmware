#pragma once

#include <EpdFontFamily.h>

#include <functional>
#include <memory>

#include "CrossPointSettings.h"
#include "components/themes/BaseTheme.h"
#include "pocket_daily/live_studio/UiPack.h"

class UITheme {
  // Static instance
  static UITheme instance;

 public:
  UITheme();
  static UITheme& getInstance() { return instance; }

  const ThemeMetrics& getMetrics() const { return *currentMetrics; }
  const BaseTheme& getTheme() const { return *currentTheme; }
  Rect getScreenSafeArea(const GfxRenderer& renderer, bool hasFrontButtonHints = false,
                         bool hasSideButtonHints = false);
  static void drawCenteredText(const GfxRenderer& renderer, Rect screen, int fontId, int y, const char* text,
                               bool black = true, EpdFontFamily::Style style = EpdFontFamily::REGULAR);
  void reload();
  void setTheme(CrossPointSettings::UI_THEME type);
  // Live Studio LS-3: apply a validated .uipack's theme overrides on top of
  // the current theme. An empty list reverts to the theme's own metrics.
  void applyPackMetrics(const PocketDaily::LiveStudio::ThemeOverride* overrides, size_t count);
  bool packActive() const { return packOverrideCount > 0; }
  static int getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasTabBar, bool hasButtonHints,
                                     bool hasSubtitle, int extraReservedHeight = 0);
  static std::string getCoverThumbPath(std::string coverBmpPath, int coverHeight);
  static UIIcon getFileIcon(const std::string& filename);
  static int getStatusBarHeight();
  static int getProgressBarHeight();

 private:
  void reapplyPackAfterThemeChange();

  const ThemeMetrics* currentMetrics;
  std::unique_ptr<BaseTheme> currentTheme;
  ThemeMetrics packedMetrics{};
  PocketDaily::LiveStudio::ThemeOverride packOverrides[PocketDaily::LiveStudio::UIPACK_MAX_THEME_OVERRIDES]{};
  size_t packOverrideCount = 0;
};

// Helper macro to access current theme
#define GUI UITheme::getInstance().getTheme()
