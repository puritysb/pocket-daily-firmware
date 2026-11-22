#include "HomeDrawing.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "fontIds.h"
#include "pocket_daily/home/GlanceFormat.h"

// Moved unchanged from PocketDailyActivity.cpp (P1-3, docs/pocket-profile-v1.md)
// so the device and the host preview draw Home and the Daily Brief with the same
// code. Hardware and font choice arrive as arguments instead of globals.
namespace PocketDaily::HomeDraw {
enum class WeatherGlyph : uint8_t { Clear, PartlyCloudy, Cloud, Fog, Rain, Snow, Storm, Unknown };

WeatherGlyph weatherGlyphFor(int16_t code, const char* summary) {
  if (code == 0) return WeatherGlyph::Clear;
  if (code == 1 || code == 2) return WeatherGlyph::PartlyCloudy;
  if (code == 3) return WeatherGlyph::Cloud;
  if (code == 45 || code == 48) return WeatherGlyph::Fog;
  if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return WeatherGlyph::Rain;
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) return WeatherGlyph::Snow;
  if (code >= 95 && code <= 99) return WeatherGlyph::Storm;
  // Older daemons omitted WMO code. Preserve a useful local glyph for their
  // short English summary without making network images a dependency.
  if (summary) {
    if (strstr(summary, "Thunder") || strstr(summary, "Storm")) return WeatherGlyph::Storm;
    if (strstr(summary, "Snow")) return WeatherGlyph::Snow;
    if (strstr(summary, "Rain") || strstr(summary, "Drizzle") || strstr(summary, "Shower")) return WeatherGlyph::Rain;
    if (strstr(summary, "Fog") || strstr(summary, "Mist")) return WeatherGlyph::Fog;
    if (strstr(summary, "Cloud") || strstr(summary, "Overcast")) return WeatherGlyph::Cloud;
    if (strstr(summary, "Clear") || strstr(summary, "Fair") || strstr(summary, "Sunny")) return WeatherGlyph::Clear;
  }
  return WeatherGlyph::Unknown;
}

void drawCircleOutline(const GfxRenderer& renderer, int cx, int cy, int radius, int stroke) {
  renderer.drawArc(radius, cx, cy, -1, -1, stroke, true);
  renderer.drawArc(radius, cx, cy, 1, -1, stroke, true);
  renderer.drawArc(radius, cx, cy, -1, 1, stroke, true);
  renderer.drawArc(radius, cx, cy, 1, 1, stroke, true);
}

void drawSunGlyph(const GfxRenderer& renderer, int cx, int cy, int radius, int stroke) {
  // A complete eight-ray mark keeps the restrained outline language of the
  // Pocket face while surviving both small forecast cells and X3 fast refresh.
  drawCircleOutline(renderer, cx, cy, radius, stroke);
  const int ray0 = radius + std::max(3, stroke);
  const int ray1 = ray0 + std::max(6, radius / 2);
  renderer.drawLine(cx, cy - ray0, cx, cy - ray1, stroke, true);
  renderer.drawLine(cx, cy + ray0, cx, cy + ray1, stroke, true);
  renderer.drawLine(cx - ray0, cy, cx - ray1, cy, stroke, true);
  renderer.drawLine(cx + ray0, cy, cx + ray1, cy, stroke, true);
  const int d0 = radius * 3 / 4 + std::max(2, stroke);
  const int d1 = d0 + std::max(4, radius / 3);
  renderer.drawLine(cx - d0, cy - d0, cx - d1, cy - d1, stroke, true);
  renderer.drawLine(cx + d0, cy - d0, cx + d1, cy - d1, stroke, true);
  renderer.drawLine(cx - d0, cy + d0, cx - d1, cy + d1, stroke, true);
  renderer.drawLine(cx + d0, cy + d0, cx + d1, cy + d1, stroke, true);
}

int drawCloudGlyph(const GfxRenderer& renderer, int x, int y, int w, int h, int stroke) {
  const int baseY = y + h * 62 / 100;
  const int left = x + w * 13 / 100;
  const int right = x + w * 88 / 100;
  const int smallR = std::max(4, std::min(w, h) * 15 / 100);
  const int bigR = std::max(6, std::min(w, h) * 23 / 100);
  const int c1x = left + smallR;
  const int c2x = x + w * 51 / 100;
  const int c3x = right - smallR;
  renderer.drawArc(smallR, c1x, baseY, -1, -1, stroke, true);
  renderer.drawArc(smallR, c1x, baseY, 1, -1, stroke, true);
  renderer.drawArc(bigR, c2x, baseY, -1, -1, stroke, true);
  renderer.drawArc(bigR, c2x, baseY, 1, -1, stroke, true);
  renderer.drawArc(smallR, c3x, baseY, -1, -1, stroke, true);
  renderer.drawArc(smallR, c3x, baseY, 1, -1, stroke, true);
  renderer.drawLine(left, baseY, right, baseY, stroke, true);
  return baseY;
}

void drawWeatherGlyph(const GfxRenderer& renderer, int x, int y, int w, int h, int16_t code, const char* summary) {
  if (w < 24 || h < 24) return;
  const WeatherGlyph glyph = weatherGlyphFor(code, summary);
  const int minDimension = std::min(w, h);
  const int stroke = std::max(2, (minDimension + 15) / 20);
  if (glyph == WeatherGlyph::Clear) {
    drawSunGlyph(renderer, x + w / 2, y + h / 2, minDimension / 4, stroke);
    return;
  }

  if (glyph == WeatherGlyph::PartlyCloudy || glyph == WeatherGlyph::Unknown) {
    drawSunGlyph(renderer, x + w * 68 / 100, y + h * 29 / 100, std::max(4, minDimension / 6), stroke);
  }
  const int baseY = drawCloudGlyph(renderer, x, y + h / 12, w, h * 3 / 4, stroke);
  if (glyph == WeatherGlyph::Fog) {
    renderer.drawLine(x + w / 5, baseY + h / 8, x + w * 4 / 5, baseY + h / 8, stroke, true);
    renderer.drawLine(x + w / 3, baseY + h / 4, x + w * 5 / 6, baseY + h / 4, stroke, true);
  } else if (glyph == WeatherGlyph::Rain) {
    for (int i = 0; i < 3; i++) {
      const int rx = x + w * (28 + i * 22) / 100;
      renderer.drawLine(rx, baseY + 7, rx - w / 15, baseY + h / 5, stroke, true);
    }
  } else if (glyph == WeatherGlyph::Snow) {
    for (int i = 0; i < 3; i++) {
      const int sx = x + w * (27 + i * 23) / 100;
      const int sy = baseY + h / 7;
      const int arm = std::max(3, minDimension / 11);
      renderer.drawLine(sx - arm, sy, sx + arm, sy, stroke, true);
      renderer.drawLine(sx, sy - arm, sx, sy + arm, stroke, true);
    }
  } else if (glyph == WeatherGlyph::Storm) {
    const int bx = x + w / 2;
    const int by = baseY + 4;
    const int xs[] = {bx + 4, bx - 6, bx + 1, bx - 8};
    const int ys[] = {by, by + h / 8, by + h / 8, by + h / 3};
    renderer.drawLine(xs[0], ys[0], xs[1], ys[1], stroke + 1, true);
    renderer.drawLine(xs[1], ys[1], xs[2], ys[2], stroke + 1, true);
    renderer.drawLine(xs[2], ys[2], xs[3], ys[3], stroke + 1, true);
  }
}

// ── Poster numerals ─────────────────────────────────────────────────────────
// The largest bundled face is 18 px — far too small for the temperature to
// anchor the panel the way the approved concept does. A display font is the
// obvious answer and the wrong one here: glyph rendering data is cached in
// DRAM on first use, and this device has ~26 KB of heap left once the radio is
// up. So the number is stroked directly instead. Every edge is axis-aligned,
// which is exactly what a 1-bit panel renders best — no anti-aliasing to smear
// at large sizes, and it costs neither flash nor DRAM.
//
// Segments are the classic seven, drawn with square joins and heavy stems so
// the result reads as condensed display type rather than a calculator.
constexpr int POSTER_DIGIT_W_PCT = 54;    // digit width as % of cap height
constexpr int POSTER_DIGIT_GAP_PCT = 14;  // inter-digit gap as % of cap height
constexpr int POSTER_STROKE_PCT = 30;     // stem weight as % of digit width

void drawPosterDigit(const GfxRenderer& renderer, int x, int y, int w, int h, int stroke, char digit) {
  // A=top B=upper-right C=lower-right D=bottom E=lower-left F=upper-left G=middle
  static const char* const kSegments[10] = {
      "ABCDEF",   // 0
      "BC",       // 1
      "ABDEG",    // 2
      "ABCDG",    // 3
      "BCFG",     // 4
      "ACDFG",    // 5
      "ACDEFG",   // 6
      "ABC",      // 7
      "ABCDEFG",  // 8
      "ABCDFG",   // 9
  };
  if (digit == '-') {  // minus keeps the middle bar only, inset like a real dash
    renderer.fillRect(x + w / 6, y + (h - stroke) / 2, w - w / 3, stroke, true);
    return;
  }
  if (digit < '0' || digit > '9') return;

  // A bare "BC" would hug the right edge of its cell and read as a gap. Centre
  // the single stem so "1" sits where the eye expects it.
  if (digit == '1') {
    renderer.fillRect(x + (w - stroke) / 2, y, stroke, h, true);
    return;
  }

  const char* segments = kSegments[digit - '0'];
  const int right = x + w - stroke;
  const int midY = y + (h - stroke) / 2;
  const int bottom = y + h - stroke;
  for (const char* s = segments; *s; ++s) {
    switch (*s) {
      case 'A':
        renderer.fillRect(x, y, w, stroke, true);
        break;
      case 'B':
        renderer.fillRect(right, y, stroke, midY - y + stroke, true);
        break;
      case 'C':
        renderer.fillRect(right, midY, stroke, bottom - midY + stroke, true);
        break;
      case 'D':
        renderer.fillRect(x, bottom, w, stroke, true);
        break;
      case 'E':
        renderer.fillRect(x, midY, stroke, bottom - midY + stroke, true);
        break;
      case 'F':
        renderer.fillRect(x, y, stroke, midY - y + stroke, true);
        break;
      case 'G':
        renderer.fillRect(x, midY, w, stroke, true);
        break;
      default:
        break;
    }
  }
}

int posterNumberWidth(int capHeight, const char* digits) {
  const int dw = capHeight * POSTER_DIGIT_W_PCT / 100;
  const int gap = capHeight * POSTER_DIGIT_GAP_PCT / 100;
  int width = 0;
  for (const char* c = digits; *c; ++c) width += (width ? gap : 0) + dw;
  return width;
}

// Draws the number and returns its width. The degree ring is drawn by the
// caller so it can sit against the cap line rather than the digit baseline.
int drawPosterNumber(const GfxRenderer& renderer, int x, int y, int capHeight, const char* digits) {
  const int dw = capHeight * POSTER_DIGIT_W_PCT / 100;
  const int gap = capHeight * POSTER_DIGIT_GAP_PCT / 100;
  const int stroke = std::max(3, dw * POSTER_STROKE_PCT / 100);
  int cursor = x;
  for (const char* c = digits; *c; ++c) {
    drawPosterDigit(renderer, cursor, y, dw, capHeight, stroke, *c);
    cursor += dw + gap;
  }
  return cursor - gap - x;
}

// Hero row: place-scale temperature on the left, condition and today's range on
// the right. Replaces the old icon+18px+H/L cluster, which competed with the
// forecast strip instead of leading it.
int drawWeatherPoster(const GfxRenderer& renderer, const PocketDaily::Weather& weather, int x, int y, int width,
                      int maxHeight) {
  if (width < 150 || maxHeight < 52) return 0;

  char digits[8] = {0};
  if (weather.tempC != PocketDaily::GLANCE_TEMP_NONE)
    snprintf(digits, sizeof(digits), "%d", (int)weather.tempC);
  else
    snprintf(digits, sizeof(digits), "--");

  // Today's rain window is the single most actionable fact on this panel, so it
  // gets the line directly under the number rather than a column of its own.
  char rain[40] = {0};
  const bool hasRain = GlanceFormat::formatRainLine(rain, sizeof(rain), weather) > 0;
  const int rainH = hasRain ? renderer.getLineHeight(SMALL_FONT_ID) + 4 : 0;
  const int heroH = maxHeight - rainH;
  // Runtime font metrics can make the rain line taller than cppcheck infers.
  // cppcheck-suppress knownConditionTrueFalse
  if (heroH < 44) return 0;

  // Size the number to the room available, then shrink until the right-hand
  // column still has a readable width.
  constexpr int kRightMinW = 96;
  int cap = std::min(96, heroH - 4);
  while (cap > 40 && posterNumberWidth(cap, digits) + cap / 4 + kRightMinW > width) cap -= 4;
  const int numberW = posterNumberWidth(cap, digits);
  drawPosterNumber(renderer, x, y, cap, digits);

  // Degree ring rides the cap line, never the baseline.
  const int degD = std::max(8, cap / 5);
  const int degStroke = std::max(2, degD / 4);
  renderer.drawRoundedRect(x + numberW + degD / 3, y, degD, degD, degStroke, degD / 2, true);

  const int rightX = x + numberW + degD / 3 + degD + cap / 5;
  const int rightW = x + width - rightX;
  if (rightW >= 60) {
    // Use the available cap height for the condition itself. The old ring and
    // one-third-column cap reduced the meaningful mark to roughly 45 px on X3.
    const int iconD = std::min(cap * 4 / 5, std::max(42, rightW / 2));
    drawWeatherGlyph(renderer, rightX, y, iconD, iconD, weather.code, weather.summary);

    int ry = y + iconD + 4;
    if (weather.summary[0] && ry + renderer.getLineHeight(SMALL_FONT_ID) <= y + heroH) {
      renderer.drawText(SMALL_FONT_ID, rightX, ry,
                        renderer.truncatedText(SMALL_FONT_ID, weather.summary, rightW, EpdFontFamily::BOLD).c_str(),
                        true, EpdFontFamily::BOLD);
      ry += renderer.getLineHeight(SMALL_FONT_ID) + 2;
    }
    if (weather.todayMaxC != PocketDaily::GLANCE_TEMP_NONE && weather.todayMinC != PocketDaily::GLANCE_TEMP_NONE) {
      char hi[12];
      char lo[12];
      snprintf(hi, sizeof(hi), "H %d\xC2\xB0", (int)weather.todayMaxC);
      snprintf(lo, sizeof(lo), "L %d\xC2\xB0", (int)weather.todayMinC);
      if (ry + renderer.getLineHeight(UI_10_FONT_ID) <= y + heroH) {
        renderer.drawText(UI_10_FONT_ID, rightX, ry, hi, true, EpdFontFamily::BOLD);
        ry += renderer.getLineHeight(UI_10_FONT_ID);
      }
      if (ry + renderer.getLineHeight(UI_10_FONT_ID) <= y + heroH)
        renderer.drawText(UI_10_FONT_ID, rightX, ry, lo, true, EpdFontFamily::BOLD);
    }
  }

  if (hasRain) {
    const int rainFont = SMALL_FONT_ID;
    renderer.drawText(rainFont, x, y + heroH + 2,
                      renderer.truncatedText(rainFont, rain, width, EpdFontFamily::BOLD).c_str(), true,
                      EpdFontFamily::BOLD);
  }
  return maxHeight;
}

// Placeholder for a device that has never received weather. The old empty
// state was a single small line pinned to the top-left of a ~150 px card, so
// the panel read as broken rather than as waiting. Same grammar as the
// populated poster — ringed mark, one honest line, the action that fixes it —
// centred so the reserved space looks deliberate.
void drawWeatherPlaceholder(const GfxRenderer& renderer, int x, int y, int width, int height, const char* noWeather,
                            const char* hint, const FontResolver& fonts) {
  const int labelFont = fonts.resolve(noWeather, SMALL_FONT_ID, EpdFontFamily::BOLD);
  const int hintFont = fonts.resolve(hint, UI_10_FONT_ID);
  if (width < 80 || height < 40) {
    renderer.drawText(labelFont, x, y, noWeather, true);
    return;
  }
  const int ringD = std::min(46, std::max(26, height / 3));
  const int lineH = renderer.getLineHeight(labelFont);
  const int hintH = renderer.getLineHeight(hintFont);
  const int blockH = ringD + 8 + lineH + 2 + hintH;
  int cy = y + std::max(0, (height - blockH) / 2);
  const int cx = x + width / 2;

  const int ring = std::max(2, ringD / 16);
  renderer.drawRoundedRect(cx - ringD / 2, cy, ringD, ringD, ring, ringD / 2, true);
  // A neutral dash, not a condition glyph: we are not claiming a forecast.
  renderer.drawRect(cx - ringD / 5, cy + ringD / 2 - ring / 2, ringD * 2 / 5, std::max(2, ring), true);
  cy += ringD + 8;

  const char* label = noWeather;
  int tw = renderer.getTextWidth(labelFont, label, EpdFontFamily::BOLD);
  renderer.drawText(labelFont, cx - tw / 2, cy, label, true, EpdFontFamily::BOLD);
  cy += lineH + 2;

  tw = renderer.getTextWidth(hintFont, hint);
  renderer.drawText(hintFont, cx - tw / 2, cy, hint, true);
}

// Five-day grid. Each column is weekday / condition / high-low / rain, ruled
// apart. The wettest day is inverted rather than annotated: on 1-bit e-ink a
// filled chip is the only emphasis that survives at a glance, and it answers
// "what should I actually notice today" without spending a line of prose.
int drawForecastGrid(const GfxRenderer& renderer, const PocketDaily::Weather& weather, int x, int y, int width,
                     int maxHeight) {
  const int count = std::min<int>(weather.dayCount, PocketDaily::WEATHER_DAY_CAP);
  if (count < 2 || width < count * 44 || maxHeight < 74) return 0;

  const int dayLine = renderer.getLineHeight(SMALL_FONT_ID);
  const int tempLine = renderer.getLineHeight(UI_10_FONT_ID);
  const int colW = width / count;
  const int glyphSize = std::min(38, std::max(26, colW * 55 / 100));
  const int height = std::min(maxHeight, dayLine + glyphSize + tempLine * 2 + 20);

  // Emphasise the wettest day, but only when it is actually worth a warning.
  int notable = -1;
  int notableRain = 39;
  for (int i = 0; i < count; i++) {
    if (weather.days[i].rainProbability > notableRain) {
      notableRain = weather.days[i].rainProbability;
      notable = i;
    }
  }

  renderer.drawLine(x, y, x + width, y);
  const int chipTop = y + 3;
  const int chipH = dayLine + 2;
  const int glyphY = chipTop + chipH + 4;
  const int tempY = glyphY + glyphSize + 3;
  const int rainY = tempY + tempLine + 2;

  for (int i = 0; i < count; i++) {
    const PocketDaily::DayWeather& day = weather.days[i];
    const int left = x + i * colW;
    const int right = i == count - 1 ? x + width : left + colW;
    const int cx = left + (right - left) / 2;
    const bool highlight = i == notable;
    if (i > 0) renderer.drawLine(left, y + 2, left, y + height - 2);

    char weekday[4] = {0};
    if (!GlanceFormat::formatWeekday(weekday, sizeof(weekday), day.date))
      snprintf(weekday, sizeof(weekday), "D%d", i + 1);
    const int dw = renderer.getTextWidth(SMALL_FONT_ID, weekday, EpdFontFamily::BOLD);
    if (highlight) renderer.fillRect(cx - dw / 2 - 6, chipTop, dw + 12, chipH, true);
    renderer.drawText(SMALL_FONT_ID, cx - dw / 2, chipTop + 1, weekday, !highlight, EpdFontFamily::BOLD);

    drawWeatherGlyph(renderer, cx - glyphSize / 2, glyphY, glyphSize, glyphSize, day.code, day.summary);

    char temps[18] = {0};
    if (day.minC != PocketDaily::GLANCE_TEMP_NONE && day.maxC != PocketDaily::GLANCE_TEMP_NONE)
      snprintf(temps, sizeof(temps), "%d\xC2\xB0/%d\xC2\xB0", (int)day.maxC, (int)day.minC);
    else if (day.maxC != PocketDaily::GLANCE_TEMP_NONE)
      snprintf(temps, sizeof(temps), "%d\xC2\xB0", (int)day.maxC);
    if (temps[0]) {
      const int tw = renderer.getTextWidth(UI_10_FONT_ID, temps, EpdFontFamily::BOLD);
      renderer.drawText(UI_10_FONT_ID, cx - tw / 2, tempY, temps, true, EpdFontFamily::BOLD);
    }

    if (day.rainProbability >= 0 && rainY + tempLine <= y + height) {
      char rain[8];
      snprintf(rain, sizeof(rain), "%d%%", (int)day.rainProbability);
      const int rw = renderer.getTextWidth(UI_10_FONT_ID, rain, EpdFontFamily::BOLD);
      if (highlight) renderer.fillRect(cx - rw / 2 - 6, rainY - 1, rw + 12, tempLine + 2, true);
      renderer.drawText(UI_10_FONT_ID, cx - rw / 2, rainY, rain, !highlight, EpdFontFamily::BOLD);
    }
  }
  return height;
}

// Solid shapes keep their identity on a one-bit panel. All geometry is
// expressed on the same 32-unit square for both current and forecast icons.
static void drawCondition(const GfxRenderer& r, int x, int y, int size, int16_t code, const char* summary) {
  auto unit = [size](int v) { return (v * size + 16) / 32; };
  auto pill = [&](int left, int top, int width, int height, int radius, Color color) {
    r.fillRoundedRect(x + unit(left), y + unit(top), unit(width), unit(height), unit(radius), color);
  };
  auto stroke = [&](int x1, int y1, int x2, int y2, int weight) {
    const bool steep = std::abs(y2 - y1) > std::abs(x2 - x1);
    const int thickness = std::max(1, unit(weight));
    for (int i = 0; i < thickness; ++i) {
      const int offset = i - thickness / 2;
      const int dx = steep ? offset : 0, dy = steep ? 0 : offset;
      r.drawLine(x + unit(x1) + dx, y + unit(y1) + dy, x + unit(x2) + dx, y + unit(y2) + dy);
    }
  };
  const auto glyph = weatherGlyphFor(code, summary);
  if (glyph == WeatherGlyph::Unknown) {
    stroke(10, 16, 22, 16, 3);
    return;
  }
  if (glyph == WeatherGlyph::Clear) {
    pill(9, 9, 14, 14, 7, Color::Black);
    stroke(16, 1, 16, 6, 3);
    stroke(16, 26, 16, 31, 3);
    stroke(1, 16, 6, 16, 3);
    stroke(26, 16, 31, 16, 3);
    stroke(5, 5, 9, 9, 3);
    stroke(23, 23, 27, 27, 3);
    stroke(5, 27, 9, 23, 3);
    stroke(23, 9, 27, 5, 3);
    return;
  }
  const int top = glyph == WeatherGlyph::Cloud ? 7 : glyph == WeatherGlyph::PartlyCloudy ? 12 : 2;
  if (glyph == WeatherGlyph::PartlyCloudy) {
    pill(16, 5, 12, 12, 6, Color::Black);
    stroke(22, 0, 22, 3, 2);
    stroke(28, 5, 30, 3, 2);
    stroke(29, 11, 31, 11, 2);
    pill(5, top - 2, 19, 16, 8, Color::White);
  }
  pill(1, top + 6, 30, 11, 5, Color::Black);
  pill(7, top, 16, 16, 8, Color::Black);
  switch (glyph) {
    case WeatherGlyph::Rain:
      for (int left : {8, 16, 24}) stroke(left + 2, 22, left - 2, 29, 3);
      break;
    case WeatherGlyph::Snow:
      for (int left : {8, 22}) {
        stroke(left, 21, left, 30, 2);
        stroke(left - 4, 23, left + 4, 28, 2);
        stroke(left - 4, 28, left + 4, 23, 2);
      }
      break;
    case WeatherGlyph::Fog:
      stroke(3, 23, 25, 23, 3);
      stroke(8, 29, 29, 29, 3);
      break;
    case WeatherGlyph::Storm:
      stroke(19, 19, 12, 25, 4);
      stroke(12, 25, 20, 25, 3);
      stroke(20, 25, 13, 31, 3);
      break;
    default:
      break;
  }
}

static void temperatureRange(char* out, size_t capacity, int high, int low) {
  if (high != GLANCE_TEMP_NONE && low != GLANCE_TEMP_NONE)
    snprintf(out, capacity, "%d°/%d°", high, low);
  else if (high != GLANCE_TEMP_NONE)
    snprintf(out, capacity, "H %d°", high);
  else if (low != GLANCE_TEMP_NONE)
    snprintf(out, capacity, "L %d°", low);
  else
    out[0] = '\0';
}

static void weatherText(const GfxRenderer& r, const char* text, int font, int x, int y, int width,
                        EpdFontFamily::Style style, bool centered) {
  if (width <= 0 || !text[0]) return;
  char clipped[48];
  snprintf(clipped, sizeof(clipped), "%s", text);
  int length = r.getTextWidth(font, clipped, style);
  while (clipped[0] && length > width) {
    removeLastUtf8(clipped);
    length = r.getTextWidth(font, clipped, style);
  }
  r.drawText(font, centered ? x - length / 2 : x, y, clipped, true, style);
}

int drawWeatherPanel(const GfxRenderer& r, const Weather& weather, int x, int y, int width, int height,
                     const FontResolver& fonts) {
  if (height < 36 || width < 120) return 0;
  const int days = std::min<int>(weather.dayCount, WEATHER_DAY_CAP);
  const int line = r.getLineHeight(UI_12_FONT_ID);
  const int small = r.getLineHeight(UI_10_FONT_ID);
  const bool list = days > 0 && height >= 135 + days * std::max(44, line + 8);
  const int count = list ? days : std::min(days, width / 66);
  const int minGrid = small * 2 + 33;
  const bool grid = !list && count > 0 && height >= 68 + minGrid;
  const int hero = list   ? std::min(160, height - days * std::max(44, line + 8))
                   : grid ? std::min(128, std::max(68, height - (minGrid + 48)))
                          : std::min(146, height);
  const int icon = std::min({hero - 6, width / 4, 128});
  drawCondition(r, x, y + (hero - icon) / 2, icon, weather.code, weather.summary);
  char digits[6];
  if (weather.tempC == GLANCE_TEMP_NONE)
    snprintf(digits, sizeof(digits), "--");
  else
    snprintf(digits, sizeof(digits), "%d", weather.tempC);
  int cap = std::clamp(std::min(hero - 12, width / 5), 16, 94);
  while (cap > 16 && icon + 22 + posterNumberWidth(cap, digits) + cap / 5 + width / 3 > width) cap -= 2;
  const int numberX = x + icon + 10;
  const int numberY = y + (hero - cap) / 2;
  const int numberW = drawPosterNumber(r, numberX, numberY, cap, digits);
  const int degree = std::max(5, cap / 5);
  if (weather.tempC != GLANCE_TEMP_NONE)
    r.drawRoundedRect(numberX + numberW + 2, numberY, degree, degree, std::max(2, degree / 4), degree / 2, true);

  const int infoX = numberX + numberW + degree + 12;
  const int infoW = x + width - infoX;
  char range[24], rain[40];
  temperatureRange(range, sizeof(range), weather.todayMaxC, weather.todayMinC);
  GlanceFormat::formatRainLine(rain, sizeof(rain), weather);
  char window[16]{};
  if ((!rain[0] || r.getTextWidth(UI_10_FONT_ID, rain) > infoW) && weather.rainProbability >= 0) {
    snprintf(rain, sizeof(rain), "Rain %d%%", weather.rainProbability);
    if (weather.rainStartHm[0])
      snprintf(window, sizeof(window), "%s%s%s", weather.rainStartHm, weather.rainEndHm[0] ? "–" : "",
               weather.rainEndHm);
  }
  const int summaryFont = fonts.resolve(weather.summary, UI_12_FONT_ID, EpdFontFamily::BOLD);
  const int summaryH = weather.summary[0] ? r.getLineHeight(summaryFont) + 2 : 0;
  int used = summaryH;
  auto fits = [&](const char* value) {
    if (!value[0] || used + small + 2 > hero || r.getTextWidth(UI_10_FONT_ID, value) > infoW) return false;
    used += small + 2;
    return true;
  };
  const bool showRange = fits(range);
  const bool showRain = fits(rain);
  const bool showWindow = fits(window);
  int textY = y + std::max(0, (hero - used) / 2);
  auto textLine = [&](int font, const char* value, EpdFontFamily::Style style) {
    weatherText(r, value, font, infoX, textY, infoW, style, false);
    textY += r.getLineHeight(font) + 2;
  };
  if (summaryH) textLine(summaryFont, weather.summary, EpdFontFamily::BOLD);
  if (showRange) textLine(UI_10_FONT_ID, range, EpdFontFamily::REGULAR);
  if (showRain) textLine(UI_10_FONT_ID, rain, EpdFontFamily::REGULAR);
  if (showWindow) textLine(UI_10_FONT_ID, window, EpdFontFamily::REGULAR);
  if (!grid && !list) return hero;

  const int row = list ? std::min(88, (height - hero) / count) : std::min(144, height - hero);
  const bool rainRow = list || row >= minGrid + small + 4;
  const int symbol =
      list ? std::min(64, row - 8) : std::min({68, width / count - 8, row - 12 - small * (rainRow ? 3 : 2)});
  for (int i = 0; i < count; ++i) {
    const auto& day = weather.days[i];
    const int top = y + hero + (list ? i * row : 0);
    const int left = x + (list ? 0 : i * width / count);
    const int cell = list ? width : (i + 1) * width / count - i * width / count;
    auto center = [&](const char* value, int font, int cx, int ty, int maxWidth, EpdFontFamily::Style style) {
      weatherText(r, value, font, cx, ty, maxWidth, style, true);
    };
    if (list || i == 0) r.drawLine(x, top, x + width - 1, top);
    const int mid = left + cell / 2;
    const int baseline = top + (row - line) / 2;
    char weekday[5] = "--";
    if (!GlanceFormat::formatWeekday(weekday, sizeof(weekday), day.date))
      snprintf(weekday, sizeof(weekday), "D%d", i + 1);
    center(weekday, list ? UI_12_FONT_ID : UI_10_FONT_ID, list ? x + width / 10 : mid, list ? baseline : top + 4, cell,
           EpdFontFamily::BOLD);
    const int iconY = list ? top + (row - symbol) / 2 : top + 6 + small;
    drawCondition(r, (list ? x + width / 3 : mid) - symbol / 2, iconY, symbol, day.code, day.summary);
    temperatureRange(range, sizeof(range), day.maxC, day.minC);
    center(range, list ? UI_12_FONT_ID : UI_10_FONT_ID, list ? x + width * 3 / 5 : mid,
           list ? baseline : iconY + symbol + 2, list ? width / 3 : cell - 2, EpdFontFamily::BOLD);
    if (rainRow && day.rainProbability >= 0) {
      snprintf(rain, sizeof(rain), "%d%%", day.rainProbability);
      center(rain, UI_10_FONT_ID, list ? x + width * 9 / 10 : mid,
             list ? top + (row - small) / 2 : iconY + symbol + small + 4, list ? width / 5 : cell,
             EpdFontFamily::REGULAR);
    }
  }
  return hero + (list ? row * count : row);
}

struct PocketHardwareGeometry {
  int frontCenters[4];
  int previousSideY;
  int nextSideY;
};

const PocketHardwareGeometry& pocketHardwareGeometry(const bool isX3) {
  // Physical portrait-panel coordinates: X3 has two front rockers and
  // opposed side keys; X4 has four narrower front keys plus a right-side
  // power/page stack. Do not replace these with framebuffer fractions.
  static constexpr PocketHardwareGeometry x3{{91, 207, 321, 437}, 194, 194};
  static constexpr PocketHardwareGeometry x4{{78, 183, 298, 403}, 385, 465};
  return isX3 ? x3 : x4;
}

void drawPocketSideChevrons(GfxRenderer& renderer, const bool isX3) {
  const auto original = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  const PocketHardwareGeometry& hardware = pocketHardwareGeometry(isX3);
  const int halfH = 10;
  const int inset = 5;
  const int arm = 11;
  const int right = renderer.getScreenWidth() - 1;
  if (isX3) {
    renderer.drawLine(inset + arm, hardware.previousSideY - halfH, inset, hardware.previousSideY, 2, true);
    renderer.drawLine(inset, hardware.previousSideY, inset + arm, hardware.previousSideY + halfH, 2, true);
    renderer.drawLine(right - inset - arm, hardware.nextSideY - halfH, right - inset, hardware.nextSideY, 2, true);
    renderer.drawLine(right - inset, hardware.nextSideY, right - inset - arm, hardware.nextSideY + halfH, 2, true);
  } else {
    renderer.drawLine(right - inset, hardware.previousSideY - halfH, right - inset - arm, hardware.previousSideY, 2,
                      true);
    renderer.drawLine(right - inset - arm, hardware.previousSideY, right - inset, hardware.previousSideY + halfH, 2,
                      true);
    renderer.drawLine(right - inset - arm, hardware.nextSideY - halfH, right - inset, hardware.nextSideY, 2, true);
    renderer.drawLine(right - inset, hardware.nextSideY, right - inset - arm, hardware.nextSideY + halfH, 2, true);
  }
  renderer.setOrientation(original);
}

bool formatWeatherSnapshotDate(char* out, size_t cap, const PocketDaily::Weather& weather) {
  if (!out || cap == 0) return false;
  out[0] = '\0';
  const char* iso = weather.dayCount > 0 ? weather.days[0].date : weather.tomorrow.date;
  if (!iso || strlen(iso) != 10 || iso[4] != '-' || iso[7] != '-') return false;
  snprintf(out, cap, "%c%c.%c%c", iso[5], iso[6], iso[8], iso[9]);
  return true;
}

bool snapshotIsStale(uint32_t savedEpoch, const time_t now) {
  return savedEpoch != 0 && now >= 1700000000 && (uint32_t)now > savedEpoch && (uint32_t)now - savedEpoch > 36 * 3600UL;
}

void drawPocketActionStrip(GfxRenderer& renderer, const bool isX3, const FontResolver& fonts, const char* first,
                           const char* second, const char* third, const char* fourth) {
  // Pocket's front controls are physical objects, not virtual rounded buttons.
  // Four edge ticks align with their real centers; labels float above only for
  // actions that exist in this context, leaving the content visually open.
  const auto original = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  const int h = renderer.getScreenHeight();
  const PocketHardwareGeometry& hardware = pocketHardwareGeometry(isX3);
  const char* labels[] = {first, second, third, fourth};
  for (int i = 0; i < 4; i++) {
    const int cx = hardware.frontCenters[i];
    renderer.drawLine(cx - 8, h - 3, cx + 8, h - 3, 2, true);
    if (!labels[i] || !labels[i][0]) continue;
    const int font = fonts.resolve(labels[i], SMALL_FONT_ID, EpdFontFamily::BOLD);
    const int textW = renderer.getTextWidth(font, labels[i], EpdFontFamily::BOLD);
    renderer.drawText(font, cx - textW / 2, h - renderer.getLineHeight(font) - 12, labels[i], true,
                      EpdFontFamily::BOLD);
  }
  renderer.setOrientation(original);
}

size_t utf8Span(const char* p) {
  if (!p || !p[0]) return 0;
  const unsigned char c = static_cast<unsigned char>(p[0]);
  size_t n = c < 0x80 ? 1 : ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : 4));
  for (size_t i = 1; i < n; i++)
    if (!p[i] || (static_cast<unsigned char>(p[i]) & 0xC0) != 0x80) return 1;
  return n;
}

void removeLastUtf8(char* text) {
  size_t n = strlen(text);
  if (n == 0) return;
  n--;
  while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80) n--;
  text[n] = '\0';
}

bool hasVisibleText(const char* p) {
  while (p && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
  return p && *p;
}

// Draw bounded UTF-8 text without std::string/vector allocations. This is used
// by every overview card during a render pass, so it must not fragment the C3's
// no-PSRAM heap. Returns the number of lines drawn.
static int wrappedFixed(const GfxRenderer& renderer, int fontId, int x, int y, const char* text, int maxWidth,
                        int maxLines, int lineAdvance, EpdFontFamily::Style style, bool draw) {
  if (!text || !text[0] || maxWidth <= 0 || maxLines <= 0) return 0;
  const char* p = text;
  int drawn = 0;
  while (hasVisibleText(p) && drawn < maxLines) {
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    char line[kWrapLineBytes];
    size_t prefix = 0;
    while (p[prefix] && p[prefix] != '\n' && p[prefix] != '\r') {
      const size_t span = utf8Span(p + prefix);
      if (!span || prefix + span >= sizeof(line)) break;
      prefix += span;
    }
    memcpy(line, p, prefix);
    line[prefix] = '\0';
    renderer.prepareUiText(fontId, line, style);
    const int fitted = renderer.getSimpleTextFitBytes(fontId, line, maxWidth, style);
    size_t scan = 0, lastFit = 0, lastSpace = 0;
    bool explicitBreak = false;
    if (fitted >= 0) {
      scan = lastFit = static_cast<size_t>(fitted);
      for (size_t i = 0; i < scan; ++i)
        if (line[i] == ' ') lastSpace = i;
      explicitBreak = p[scan] == '\n' || p[scan] == '\r';
    } else {
      line[0] = '\0';
      while (p[scan]) {
        if (p[scan] == '\n' || p[scan] == '\r') {
          explicitBreak = true;
          break;
        }
        const size_t cp = utf8Span(p + scan);
        if (cp == 0 || scan + cp >= sizeof(line)) break;
        memcpy(line + scan, p + scan, cp);
        line[scan + cp] = '\0';
        if (renderer.getTextWidth(fontId, line, style) > maxWidth) break;
        if (p[scan] == ' ') lastSpace = scan;
        scan += cp;
        lastFit = scan;
      }
    }

    size_t chosen = lastFit;
    size_t consumed = scan;
    if (p[scan] && !explicitBreak) {
      if (lastSpace > 0) {
        chosen = lastSpace;
        consumed = lastSpace + 1;
      } else {
        consumed = lastFit;
      }
    } else if (explicitBreak) {
      consumed = scan + 1;
    }
    if (chosen == 0 && p[0]) {
      chosen = utf8Span(p);
      if (chosen >= sizeof(line)) chosen = sizeof(line) - 1;
      memcpy(line, p, chosen);
      consumed = chosen;
    }
    line[chosen] = '\0';
    while (chosen > 0 && line[chosen - 1] == ' ') line[--chosen] = '\0';

    const bool more = hasVisibleText(p + consumed);
    if (draw && drawn == maxLines - 1 && more) {
      constexpr const char* dots = "...";
      while (line[0]) {
        char candidate[kWrapLineBytes];
        snprintf(candidate, sizeof(candidate), "%s%s", line, dots);
        if (renderer.getTextWidth(fontId, candidate, style) <= maxWidth) break;
        removeLastUtf8(line);
      }
      strncat(line, dots, sizeof(line) - strlen(line) - 1);
    }
    if (draw) renderer.drawText(fontId, x, y + drawn * lineAdvance, line, true, style);
    drawn++;
    if (consumed == 0) break;
    p += consumed;
  }
  return drawn;
}

int drawWrappedFixed(const GfxRenderer& renderer, int fontId, int x, int y, const char* text, int maxWidth,
                     int maxLines, int lineAdvance, EpdFontFamily::Style style) {
  return wrappedFixed(renderer, fontId, x, y, text, maxWidth, maxLines, lineAdvance, style, true);
}

int measureWrappedFixed(const GfxRenderer& renderer, int fontId, const char* text, int maxWidth, int maxLines,
                        EpdFontFamily::Style style) {
  return wrappedFixed(renderer, fontId, 0, 0, text, maxWidth, maxLines, 0, style, false);
}

}  // namespace PocketDaily::HomeDraw
