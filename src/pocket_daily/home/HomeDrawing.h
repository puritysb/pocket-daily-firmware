#pragma once

#include <EpdFontFamily.h>

#include <cstddef>
#include <cstdint>
#include <ctime>

#include "pocket_daily/models.h"

class GfxRenderer;

// Pocket Daily Home / Daily Brief drawing helpers shared by the device and the
// host preview (P1-3, docs/pocket-profile-v1.md). Pure: no SD, radio, clock or
// theme singletons; hardware and font choice are arguments.
namespace PocketDaily::HomeDraw {
// Picks the font that can draw `text` (the device swaps to an installed SD
// CJK font; the host preview to its bundled font), else `fallback`.
struct FontResolver {
  void* context = nullptr;
  int (*pick)(void* context, const char* text, int fallback, EpdFontFamily::Style style) = nullptr;
  int resolve(const char* text, const int fallback, const EpdFontFamily::Style style = EpdFontFamily::REGULAR) const {
    return pick ? pick(context, text, fallback, style) : fallback;
  }
};

inline constexpr size_t kWrapLineBytes = 192;

int drawWeatherPoster(const GfxRenderer& renderer, const PocketDaily::Weather& weather, int x, int y, int width,
                      int maxHeight);
int drawForecastGrid(const GfxRenderer& renderer, const PocketDaily::Weather& weather, int x, int y, int width,
                     int maxHeight);
void drawWeatherPlaceholder(const GfxRenderer& renderer, int x, int y, int width, int height, const char* noWeather,
                            const char* hint);
void drawPocketSideChevrons(GfxRenderer& renderer, bool isX3);
void drawPocketActionStrip(GfxRenderer& renderer, bool isX3, const FontResolver& fonts, const char* first,
                           const char* second, const char* fourth);
bool formatWeatherSnapshotDate(char* out, size_t cap, const PocketDaily::Weather& weather);
// Older than 36 hours at `now` (and the clock is plausible).
bool snapshotIsStale(uint32_t savedEpoch, time_t now);

size_t utf8Span(const char* p);
void removeLastUtf8(char* text);
bool hasVisibleText(const char* p);
// Bounded UTF-8 wrapping without heap allocation; returns lines drawn.
int drawWrappedFixed(const GfxRenderer& renderer, int fontId, int x, int y, const char* text, int maxWidth,
                     int maxLines, int lineAdvance, EpdFontFamily::Style style = EpdFontFamily::REGULAR);
}  // namespace PocketDaily::HomeDraw
