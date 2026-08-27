#include "PocketDailyActivity.h"

#include <Bitmap.h>
#include <ESPmDNS.h>
#include <EpdFontFamily.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>
#include <WiFi.h>
#include <esp_ota_ops.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "HalGPIO.h"
#include "HalPowerManager.h"
#include "PowerCycle.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "WifiCredentialStore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "agent/AgentLog.h"
#include "agentdeck/agent_commands.h"
#include "agentdeck/agent_state.h"
#include "agentdeck/auth_store.h"
#include "agentdeck/card_class.h"
#include "agentdeck/feed_client.h"
#include "agentdeck/glance_format.h"
#include "agentdeck/mdns_discovery.h"
#include "agentdeck/ota_pull.h"
#include "agentdeck/ota_ws_receiver.h"
#include "agentdeck/outbox_store.h"
#include "agentdeck/udp_discovery.h"
#include "agentdeck/ws_client.h"
#include "components/UITheme.h"  // GUI (theme) + ThemeMetrics + Rect
#include "components/icons/glyph_antigravity.h"
#include "components/icons/glyph_claude.h"
#include "components/icons/glyph_codex.h"
#include "components/icons/glyph_openclaw.h"
#include "components/icons/glyph_opencode.h"
#include "fontIds.h"
#include "pocket_daily/product_identity.h"
#include "util/PowerWakeCue.h"
#include "util/UiCjkFont.h"

namespace {
using AgentDeck::AgentState;

// esp_http_client ultimately enters lwIP's DNS path. On a cold offline boot,
// WIFI_MODE_NULL means the TCP/IP core mutex may not exist yet; attempting a
// cached-endpoint request then asserts inside xQueueSemaphoreTake instead of
// returning an ordinary transport error. Keep all Pocket HTTP entry points
// behind this shared readiness predicate.
bool wifiReadyForHttp() { return WiFi.getMode() != WIFI_MODE_NULL && WiFi.status() == WL_CONNECTED; }

enum class WeatherGlyph : uint8_t { Clear, PartlyCloudy, Cloud, Fog, Rain, Snow, Storm };

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
  return WeatherGlyph::PartlyCloudy;
}

void drawCircleOutline(const GfxRenderer& renderer, int cx, int cy, int radius, int stroke = 2) {
  renderer.drawArc(radius, cx, cy, -1, -1, stroke, true);
  renderer.drawArc(radius, cx, cy, 1, -1, stroke, true);
  renderer.drawArc(radius, cx, cy, -1, 1, stroke, true);
  renderer.drawArc(radius, cx, cy, 1, 1, stroke, true);
}

void drawSunGlyph(const GfxRenderer& renderer, int cx, int cy, int radius, int stroke) {
  drawCircleOutline(renderer, cx, cy, radius, stroke);
  const int ray0 = radius + 4;
  const int ray1 = radius + std::max(8, radius / 2);
  renderer.drawLine(cx, cy - ray0, cx, cy - ray1, stroke, true);
  renderer.drawLine(cx, cy + ray0, cx, cy + ray1, stroke, true);
  renderer.drawLine(cx - ray0, cy, cx - ray1, cy, stroke, true);
  renderer.drawLine(cx + ray0, cy, cx + ray1, cy, stroke, true);
  const int d0 = radius * 3 / 4 + 3;
  const int d1 = radius + 8;
  renderer.drawLine(cx - d0, cy - d0, cx - d1, cy - d1, stroke, true);
  renderer.drawLine(cx + d0, cy - d0, cx + d1, cy - d1, stroke, true);
}

int drawCloudGlyph(const GfxRenderer& renderer, int x, int y, int w, int h, int stroke) {
  const int baseY = y + h * 63 / 100;
  const int left = x + w * 13 / 100;
  const int right = x + w * 88 / 100;
  const int smallR = std::max(7, std::min(w, h) * 15 / 100);
  const int bigR = std::max(10, std::min(w, h) * 23 / 100);
  const int c1x = left + smallR;
  const int c2x = x + w * 51 / 100;
  const int c3x = right - smallR;
  // Only the upper halves are drawn; the shared baseline closes the cloud.
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
  const int stroke = std::max(2, std::min(w, h) / 28);
  if (glyph == WeatherGlyph::Clear) {
    drawSunGlyph(renderer, x + w / 2, y + h / 2, std::min(w, h) / 5, stroke);
    return;
  }

  if (glyph == WeatherGlyph::PartlyCloudy) {
    drawSunGlyph(renderer, x + w * 67 / 100, y + h * 31 / 100, std::min(w, h) / 7, stroke);
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
      renderer.drawLine(sx - 4, sy, sx + 4, sy, stroke, true);
      renderer.drawLine(sx, sy - 4, sx, sy + 4, stroke, true);
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

int drawCurrentWeatherVisual(const GfxRenderer& renderer, const PocketDaily::Weather& weather, int x, int y, int width,
                             int maxHeight) {
  if (width < 110 || maxHeight < 46) return 0;
  const int height = std::min(84, maxHeight);
  const int iconW = std::min(86, std::max(54, width * 35 / 100));
  drawWeatherGlyph(renderer, x, y + 1, iconW, height - 2, weather.code, weather.summary);

  const int textX = x + iconW + 10;
  const int textW = width - iconW - 10;
  char value[24] = {0};
  if (weather.tempC != PocketDaily::GLANCE_TEMP_NONE)
    snprintf(value, sizeof(value), "%d\xC2\xB0", (int)weather.tempC);
  else
    snprintf(value, sizeof(value), "--");
  renderer.drawText(NOTOSANS_18_FONT_ID, textX, y + 3,
                    renderer.truncatedText(NOTOSANS_18_FONT_ID, value, textW, EpdFontFamily::BOLD).c_str(), true,
                    EpdFontFamily::BOLD);

  char range[28] = {0};
  if (weather.todayMinC != PocketDaily::GLANCE_TEMP_NONE && weather.todayMaxC != PocketDaily::GLANCE_TEMP_NONE)
    snprintf(range, sizeof(range), "H %d\xC2\xB0  L %d\xC2\xB0", (int)weather.todayMaxC, (int)weather.todayMinC);
  else if (weather.summary[0])
    snprintf(range, sizeof(range), "%s", weather.summary);
  if (range[0]) {
    const int rangeY = y + renderer.getLineHeight(NOTOSANS_18_FONT_ID) + 8;
    renderer.drawText(SMALL_FONT_ID, textX, rangeY,
                      renderer.truncatedText(SMALL_FONT_ID, range, textW, EpdFontFamily::BOLD).c_str(), true,
                      EpdFontFamily::BOLD);
  }
  return height;
}

// Dense five-day visual for 1-bit e-ink. Each fixed column has an immediately
// recognizable condition glyph, weekday and high/low, while the shared traces
// preserve the useful trend and the bottom bars keep rain probability visual.
int drawForecastRibbon(const GfxRenderer& renderer, const PocketDaily::Weather& weather, int x, int y, int width,
                       int maxHeight) {
  const int count = std::min<int>(weather.dayCount, PocketDaily::WEATHER_DAY_CAP);
  if (count < 2 || width < count * 44 || maxHeight < 106) return 0;
  const int height = std::min(148, maxHeight);
  const int dayLine = renderer.getLineHeight(SMALL_FONT_ID);
  const int tempLine = renderer.getLineHeight(UI_10_FONT_ID);
  const int colW = width / count;
  const int glyphSize = std::min(34, std::max(26, colW * 38 / 100));
  renderer.drawLine(x, y, x + width, y);

  int minTemp = 127;
  int maxTemp = -127;
  for (int i = 0; i < count; i++) {
    const PocketDaily::DayWeather& day = weather.days[i];
    if (day.minC != PocketDaily::GLANCE_TEMP_NONE) minTemp = std::min(minTemp, (int)day.minC);
    if (day.maxC != PocketDaily::GLANCE_TEMP_NONE) maxTemp = std::max(maxTemp, (int)day.maxC);
  }
  if (minTemp > maxTemp) {
    minTemp = 0;
    maxTemp = 1;
  }
  if (maxTemp - minTemp < 4) {
    minTemp -= 2;
    maxTemp += 2;
  }

  const int glyphY = y + dayLine + 7;
  const int tempY0 = glyphY + glyphSize + 3;
  const int plotTop = tempY0 + tempLine + 7;
  const int rainBase = y + height - 5;
  const int plotBottom = std::max(plotTop + 8, rainBase - 18);
  const int plotH = std::max(1, plotBottom - plotTop);
  auto tempY = [&](int temp) { return plotTop + (maxTemp - temp) * plotH / std::max(1, maxTemp - minTemp); };

  int previousX = 0;
  int previousHighY = 0;
  int previousLowY = 0;
  bool previousHigh = false;
  bool previousLow = false;
  for (int i = 0; i < count; i++) {
    const PocketDaily::DayWeather& day = weather.days[i];
    const int left = x + i * colW;
    const int right = i == count - 1 ? x + width : left + colW;
    const int cx = left + (right - left) / 2;

    char weekday[4] = {0};
    if (!AgentDeck::GlanceFormat::formatWeekday(weekday, sizeof(weekday), day.date))
      snprintf(weekday, sizeof(weekday), "D%d", i + 1);
    int tw = renderer.getTextWidth(SMALL_FONT_ID, weekday, EpdFontFamily::BOLD);
    renderer.drawText(SMALL_FONT_ID, cx - tw / 2, y + 5, weekday, true, EpdFontFamily::BOLD);

    drawWeatherGlyph(renderer, cx - glyphSize / 2, glyphY, glyphSize, glyphSize, day.code, day.summary);

    char temps[18] = {0};
    if (day.minC != PocketDaily::GLANCE_TEMP_NONE && day.maxC != PocketDaily::GLANCE_TEMP_NONE)
      snprintf(temps, sizeof(temps), "%d/%d", (int)day.maxC, (int)day.minC);
    else if (day.maxC != PocketDaily::GLANCE_TEMP_NONE)
      snprintf(temps, sizeof(temps), "%d", (int)day.maxC);
    if (temps[0]) {
      tw = renderer.getTextWidth(UI_10_FONT_ID, temps);
      renderer.drawText(UI_10_FONT_ID, cx - tw / 2, tempY0, temps, true);
    }

    if (day.maxC != PocketDaily::GLANCE_TEMP_NONE) {
      const int highY = tempY(day.maxC);
      if (previousHigh) renderer.drawLine(previousX, previousHighY, cx, highY, 2, true);
      renderer.fillRect(cx - 2, highY - 2, 5, 5, true);
      previousHighY = highY;
      previousHigh = true;
    } else {
      previousHigh = false;
    }
    if (day.minC != PocketDaily::GLANCE_TEMP_NONE) {
      const int lowY = tempY(day.minC);
      if (previousLow) renderer.drawLine(previousX, previousLowY, cx, lowY, true);
      renderer.drawRect(cx - 2, lowY - 2, 5, 5, true);
      previousLowY = lowY;
      previousLow = true;
    } else {
      previousLow = false;
    }

    if (day.rainProbability > 0) {
      const int barH = std::max(3, std::min(18, (int)day.rainProbability * 18 / 100));
      const int barW = std::max(8, std::min(18, colW / 5));
      renderer.fillRect(cx - barW / 2, rainBase - barH, barW, barH, true);
    }
    previousX = cx;
  }
  renderer.drawLine(x, rainBase, x + width, rainBase);
  return height;
}

struct PocketHardwareGeometry {
  int frontCenters[4];
  int previousSideY;
  int nextSideY;
  int powerAxis;
  bool powerOnTop;
};

const PocketHardwareGeometry& pocketHardwareGeometry() {
  // Physical portrait-panel coordinates: X3 has two front rockers and
  // opposed side keys; X4 has four narrower front keys plus a right-side
  // power/page stack. Do not replace these with framebuffer fractions.
  static constexpr PocketHardwareGeometry x3{{91, 207, 321, 437}, 194, 194, 473, true};
  static constexpr PocketHardwareGeometry x4{{78, 183, 298, 403}, 385, 465, 74, false};
  return gpio.deviceIsX3() ? x3 : x4;
}

void drawPocketSideChevrons(GfxRenderer& renderer) {
  const auto original = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  const PocketHardwareGeometry& hardware = pocketHardwareGeometry();
  const int halfH = 10;
  const int inset = 5;
  const int arm = 11;
  const int right = renderer.getScreenWidth() - 1;
  if (gpio.deviceIsX3()) {
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

int formatNextRainDay(char* out, size_t cap, const PocketDaily::Weather& weather) {
  if (!out || cap == 0) return 0;
  out[0] = '\0';
  for (uint8_t i = 1; i < weather.dayCount && i < PocketDaily::WEATHER_DAY_CAP; i++) {
    const PocketDaily::DayWeather& day = weather.days[i];
    const WeatherGlyph glyph = weatherGlyphFor(day.code, day.summary);
    if (glyph != WeatherGlyph::Rain && glyph != WeatherGlyph::Storm && day.rainProbability <= 0) continue;
    char weekday[4] = {0};
    if (!AgentDeck::GlanceFormat::formatWeekday(weekday, sizeof(weekday), day.date))
      snprintf(weekday, sizeof(weekday), "D%d", i + 1);
    if (day.rainProbability > 0) return snprintf(out, cap, "RAIN %s %d%%", weekday, (int)day.rainProbability);
    return snprintf(out, cap, "RAIN %s", weekday);
  }
  return 0;
}

bool formatWeatherSnapshotDate(char* out, size_t cap, const PocketDaily::Weather& weather) {
  if (!out || cap == 0) return false;
  out[0] = '\0';
  const char* iso = weather.dayCount > 0 ? weather.days[0].date : weather.tomorrow.date;
  if (!iso || strlen(iso) != 10 || iso[4] != '-' || iso[7] != '-') return false;
  snprintf(out, cap, "%c%c.%c%c", iso[5], iso[6], iso[8], iso[9]);
  return true;
}

bool snapshotIsStale(uint32_t savedEpoch) {
  const time_t now = time(nullptr);
  return savedEpoch != 0 && now >= 1700000000 && (uint32_t)now > savedEpoch && (uint32_t)now - savedEpoch > 36 * 3600UL;
}

void drawPocketActionStrip(GfxRenderer& renderer, const char* first, const char* second, const char* fourth) {
  // Pocket's front controls are physical objects, not virtual rounded buttons.
  // Four edge ticks align with their real centers; labels float above only for
  // actions that exist in this context, leaving the content visually open.
  const auto original = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  const int h = renderer.getScreenHeight();
  const PocketHardwareGeometry& hardware = pocketHardwareGeometry();
  const char* labels[] = {first, second, "", fourth};
  for (int i = 0; i < 4; i++) {
    const int cx = hardware.frontCenters[i];
    renderer.drawLine(cx - 8, h - 3, cx + 8, h - 3, 2, true);
    if (!labels[i] || !labels[i][0]) continue;
    const int font = UiCjkFont::fontForText(renderer, labels[i], SMALL_FONT_ID, EpdFontFamily::BOLD);
    const int textW = renderer.getTextWidth(font, labels[i], EpdFontFamily::BOLD);
    renderer.drawText(font, cx - textW / 2, h - renderer.getLineHeight(font) - 12, labels[i], true,
                      EpdFontFamily::BOLD);
  }
  renderer.setOrientation(original);
}

// Per-agent creature glyph (src/components/icons/glyph_*.h). MUST be a multiple of
// 8: EInkDisplay::drawImageTransparent reads `width/8` bytes per row, so a width
// like 28 (→ 3 bytes, but convert_icon.py packs 4) desyncs every row into noise.
constexpr int kGlyphPx = 32;

// Canonical 1-bit creature glyph for an agent type → nullptr falls back to a dot.
// Mirrors the AGENT_MONO_GLYPH map (shared/src/svg-renderers/agent-logos.ts). Keep
// every wire agentType covered here — a missing branch silently degrades to a dot.
const uint8_t* glyphForAgent(const char* a) {
  if (!a || !a[0]) return nullptr;
  if (strcmp(a, "claude-code") == 0) return GlyphClaude;
  if (strncmp(a, "codex", 5) == 0) return GlyphCodex;  // codex-cli / codex-app / codex
  if (strcmp(a, "opencode") == 0) return GlyphOpenCode;
  if (strcmp(a, "openclaw") == 0) return GlyphOpenClaw;
  if (strncmp(a, "antigravity", 11) == 0 || strcmp(a, "agy") == 0) return GlyphAntigravity;
  return nullptr;
}

// True when the text contains Hangul / CJK / Kana codepoints — i.e. glyphs the
// Latin-only built-in UI fonts can't render (they'd show □). Used to swap to an
// installed SD CJK font for that line.
bool hasCJK(const char* s) {
  if (!s) return false;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
  while (*p) {
    const unsigned char c = *p;
    if (c < 0x80) {
      p++;
      continue;
    }
    uint32_t cp = 0;
    int n = 0;
    if ((c & 0xE0) == 0xC0) {
      cp = c & 0x1F;
      n = 1;
    } else if ((c & 0xF0) == 0xE0) {
      cp = c & 0x0F;
      n = 2;
    } else if ((c & 0xF8) == 0xF0) {
      cp = c & 0x07;
      n = 3;
    } else {
      p++;
      continue;
    }
    p++;
    for (int i = 0; i < n && *p; i++, p++) cp = (cp << 6) | (*p & 0x3F);
    if ((cp >= 0xAC00 && cp <= 0xD7A3) ||  // Hangul syllables
        (cp >= 0x1100 && cp <= 0x11FF) ||  // Hangul Jamo
        (cp >= 0x3040 && cp <= 0x30FF) ||  // Hiragana + Katakana
        (cp >= 0x4E00 && cp <= 0x9FFF))    // CJK unified ideographs
      return true;
  }
  return false;
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
int drawWrappedFixed(const GfxRenderer& renderer, int fontId, int x, int y, const char* text, int maxWidth,
                     int maxLines, int lineAdvance, EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  if (!text || !text[0] || maxWidth <= 0 || maxLines <= 0) return 0;
  const char* p = text;
  int drawn = 0;
  while (hasVisibleText(p) && drawn < maxLines) {
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    char line[AgentDeck::SESSION_ACTIVITY_CAP];
    line[0] = '\0';
    size_t scan = 0, lastFit = 0, lastSpace = 0;
    bool explicitBreak = false;
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
    if (drawn == maxLines - 1 && more) {
      constexpr const char* dots = "...";
      while (line[0]) {
        char candidate[AgentDeck::SESSION_ACTIVITY_CAP];
        snprintf(candidate, sizeof(candidate), "%s%s", line, dots);
        if (renderer.getTextWidth(fontId, candidate, style) <= maxWidth) break;
        removeLastUtf8(line);
      }
      strncat(line, dots, sizeof(line) - strlen(line) - 1);
    }
    renderer.drawText(fontId, x, y + drawn * lineAdvance, line, true, style);
    drawn++;
    if (consumed == 0) break;
    p += consumed;
  }
  return drawn;
}

// Strip an "observed:<agent>:" prefix → the raw session UUID. The sessions_list
// id for passively-observed sessions is prefixed ("observed:claude:<uuid>") while
// timeline entries are keyed by the raw UUID, so Detail must compare the raw form.
const char* rawSid(const char* sid) {
  if (sid && strncmp(sid, "observed:", 9) == 0) {
    const char* p = strchr(sid + 9, ':');
    if (p) return p + 1;
  }
  return sid ? sid : "";
}

// FNV-1a over a byte range — cheap change-detection signature.
inline uint32_t fnvUpdate(uint32_t h, const void* data, size_t len) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < len; i++) {
    h ^= p[i];
    h *= 16777619u;
  }
  return h;
}

const char* agentStateLabel(AgentState s) {
  switch (s) {
    case AgentState::IDLE:
      return "Idle";
    case AgentState::PROCESSING:
      return "Working";
    case AgentState::AWAITING_PERMISSION:
      return "Awaiting permission";
    case AgentState::AWAITING_OPTION:
      return "Choosing option";
    case AgentState::AWAITING_DIFF:
      return "Reviewing diff";
    case AgentState::DISCONNECTED:
    default:
      return "Offline";
  }
}

// Prose form of a raw wire state for the Detail meta line.
// Unknown strings pass through unchanged (already-pretty fallback labels).
const char* wireStateLabel(const char* s) {
  if (!s || !s[0]) return "";
  if (strcmp(s, "processing") == 0) return "Working";
  if (strcmp(s, "idle") == 0) return "Idle";
  if (strcmp(s, "awaiting_permission") == 0) return "Awaiting permission";
  if (strcmp(s, "awaiting_option") == 0) return "Choosing option";
  if (strcmp(s, "awaiting_diff") == 0) return "Reviewing diff";
  if (strcmp(s, "disconnected") == 0) return "Offline";
  return s;
}

// E-ink timeline marker per entry type. Vocabulary is the shared SSOT
// EINK_ICON_GLYPHS in AgentDeck shared/src/timeline-icons.ts (mirrored on the
// Android e-ink surface) — keep in lockstep when the icon-key mapping changes.
const char* timelineGlyph(const char* type) {
  if (!type || !type[0]) return "[..]";
  if (strcmp(type, "chat_start") == 0) return "[..]";
  if (strcmp(type, "chat_end") == 0 || strcmp(type, "chat_response") == 0 || strcmp(type, "model_response") == 0 ||
      strcmp(type, "tool_resolved") == 0 || strcmp(type, "task_milestone") == 0 || strcmp(type, "eval_result") == 0)
    return "[OK]";
  if (strcmp(type, "task_start") == 0 || strcmp(type, "task_end") == 0) return "[==]";
  if (strcmp(type, "error") == 0) return "[!!]";
  if (strcmp(type, "tool_request") == 0) return "[??]";
  if (strcmp(type, "tool_exec") == 0) return "[T ]";
  if (strcmp(type, "model_call") == 0) return "[M ]";
  if (strcmp(type, "user_action") == 0) return "[U ]";
  if (strcmp(type, "scheduled") == 0) return "[S ]";
  if (strcmp(type, "memory_recall") == 0) return "[~ ]";
  return "[..]";
}

// Compact age ("now" / "5m" / "2h" / "3d") from seconds. Buffer >= 6 bytes.
void formatAge(uint32_t ageSec, char* out, size_t n) {
  if (ageSec < 60)
    snprintf(out, n, "now");
  else if (ageSec < 3600)
    snprintf(out, n, "%um", (unsigned)(ageSec / 60));
  else if (ageSec < 86400)
    snprintf(out, n, "%uh", (unsigned)(ageSec / 3600));
  else
    snprintf(out, n, "%ud", (unsigned)(ageSec / 86400));
}

const char* deviceModelName() { return gpio.deviceIsX3() ? "XTeink X3" : "XTeink X4"; }

const char* deviceModelSlug() { return gpio.deviceIsX3() ? "xteink-x3" : "xteink-x4"; }

const char* mdnsHostName() { return gpio.deviceIsX3() ? "agentdeck-x3" : "agentdeck-x4"; }

struct JapaneseDailyWord {
  const char* word;
  const char* reading;
  const char* meaning;
  const char* example;
};

// Compact, device-owned starter deck. These are intentionally common words
// with short examples: one 12px NotoSansJP line can carry each entry on X3,
// and no network, account or host process is required to make Study useful.
constexpr JapaneseDailyWord kJapaneseDailyWords[] = {
    {"習慣", "しゅうかん", "habit", "毎日、本を読む習慣をつける。"},
    {"続ける", "つづける", "continue", "日本語の勉強を毎日続ける。"},
    {"気づく", "きづく", "notice", "小さな変化に気づいた。"},
    {"選ぶ", "えらぶ", "choose", "好きな本を一冊選ぶ。"},
    {"確かめる", "たしかめる", "check", "答えをもう一度確かめる。"},
    {"間に合う", "まにあう", "be in time", "電車に間に合った。"},
    {"楽しみ", "たのしみ", "look forward to", "旅行を楽しみにしている。"},
    {"振り返る", "ふりかえる", "reflect", "一日を静かに振り返る。"},
    {"身につける", "みにつける", "acquire", "新しい表現を身につける。"},
    {"試す", "ためす", "try", "別の方法を試してみる。"},
    {"集中", "しゅうちゅう", "focus", "読書に集中する。"},
    {"調べる", "しらべる", "look up", "知らない言葉を調べる。"},
    {"伝える", "つたえる", "convey", "自分の考えを伝える。"},
    {"比べる", "くらべる", "compare", "二つの表現を比べる。"},
    {"慣れる", "なれる", "get used to", "新しい環境に慣れる。"},
    {"工夫", "くふう", "devise", "時間の使い方を工夫する。"},
};

constexpr size_t kJapaneseDailyWordCount = sizeof(kJapaneseDailyWords) / sizeof(kJapaneseDailyWords[0]);
}  // namespace

void PocketDailyActivity::onEnter() {
  Activity::onEnter();

  // Pocket is a hardware-shaped shell. Normalize it to the chassis portrait
  // coordinate system so edge and front-button affordances remain true even
  // when the preceding reader page used a rotated orientation.
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  dashState = DashState::WifiSelection;
  localIp.clear();
  exitRequested = false;
  registered = false;
  lastSignature = 0;
  savedWifiJoinFailed = false;
  savedWifiScanActive = false;
  savedWifiPickerOnFailure = false;
  joiningSsid[0] = '\0';

  // Bring the networking module up from a clean slate.
  AgentDeck::ensureStateMutex();
  AgentDeck::lockState();
  AgentDeck::g_state.reset();
  AgentDeck::unlockState();
  AgentDeck::Net::wsInit();

  // Load the persisted Pocket BEFORE the first paint so boot lands on carried
  // content, not an empty screen. Its choices remain actionable through the SD
  // Outbox even when no daemon or network is present.
  cachedDeck = makeUniqueNoThrow<PocketDaily::DeckStore::Snapshot>();
  if (!cachedDeck) {
    LOG_ERR("POCKET", "OOM allocating %uB deck cache", (unsigned)sizeof(PocketDaily::DeckStore::Snapshot));
  } else if (!PocketDaily::DeckStore::load(*cachedDeck)) {
    cachedDeck->count = 0;
  }
  // Claim the persist build buffer now, while the pre-radio heap still has
  // ~82 KB contiguous. After Wi-Fi is up the largest block measures 4.6-6.9 KB
  // and this same request is the one that has been failing.
  deckScratch = makeUniqueNoThrow<PocketDaily::DeckStore::Snapshot>();
  if (!deckScratch)
    LOG_ERR("POCKET", "OOM allocating %uB deck scratch", (unsigned)sizeof(PocketDaily::DeckStore::Snapshot));
  // Pocket cards are day/info content, not live session state. Seed them into
  // RAM from the deck cache so they remain readable and answerable through the
  // SD outbox before Wi-Fi or the daemon exists.
  if (cachedDeck && cachedDeck->pocketCount > 0) {
    AgentDeck::lockState();
    AgentDeck::g_state.pocketCount = cachedDeck->pocketCount;
    memcpy(AgentDeck::g_state.pocketCards, cachedDeck->pocketCards,
           sizeof(PocketDaily::Card) * cachedDeck->pocketCount);
    AgentDeck::unlockState();
  }
  // Conditional pull: echo the sig persisted with the deck cache so a wake
  // against an unchanged deck costs one tiny response.
  lastFeedSig[0] = '\0';
  if (cachedDeck && cachedDeck->deckSig[0]) {
    strncpy(lastFeedSig, cachedDeck->deckSig, sizeof(lastFeedSig) - 1);
    lastFeedSig[sizeof(lastFeedSig) - 1] = '\0';
  }
  lastDeckSig = 0;
  lastDeckSaveMs = 0;
  clockSynced = time(nullptr) >= 1700000000;
  localStudyOffset = 0;
  buildLocalStudyCard();

  // Battery cadence: a timer wake with Pocket sync enabled syncs once over
  // HTTP and deep-sleeps again; any button press cancels into interactive mode.
  // USB power means docked — stay in the live WS mode regardless of the timer.
  enterMs = millis();
  pullMode = SETTINGS.agentPullSyncEnabled != 0 && gpio.getWakeupReason() == HalGPIO::WakeupReason::Timer &&
             !gpio.isUsbConnected();
  pullSynced = false;
  pullEndpointTried = false;
  manualSyncQueued = false;
  manualSyncActive = false;
  manualOtaResumePending = false;
  manualOtaIncrementalActive = false;
  pullOtaDownloading = false;
  pullOtaPctBucket = -1;
  manualOtaNoProgressRetries = 0;
  manualOtaResumeAtMs = 0;
  manualOtaResumeStartedMs = 0;
  pullOtaDownloadedBytes = 0;
  pullOtaTotalBytes = 0;
  lastManualOtaMd5[0] = '\0';
  manualSyncNeedsDiscovery = false;
  manualSyncDiscoveryRetryActive = false;
  glanceReason = GlanceReason::Ambient;
  sleepFramePending = false;
  pullSyncedAtMs = 0;
  pullNextSec = 0;
  if (pullMode) AgentLog::line("AGENT", "pull-sync wake (battery cadence)");

  AgentLog::line("POCKET", "Pocket reader onEnter");
  // Paint Pocket immediately — local reading and cached cards do not wait for
  // Wi-Fi, discovery, or a daemon.
  requestUpdate();

  // WifiSelectionActivity used to be the only owner that loaded this store.
  // Pocket can now join without ever opening that activity, so load the tiny
  // credential record here before starting the background scan.
  {
    RenderLock storageLock(*this);
    WIFI_STORE.loadFromFile();
  }

  if (WiFi.status() == WL_CONNECTED) {
    localIp = WiFi.localIP().toString().c_str();
    onWifiSelectionComplete(true);
    return;
  }

  // Saved credentials → background STA join, no blocking picker. The Face is
  // already on screen; loop() promotes the state when the join lands.
  if (startSavedWifiJoin()) return;

  // First run without Wi-Fi is still a complete reader. Network setup is an
  // explicit refresh action from the empty Pocket, never a boot gate.
  WiFi.mode(WIFI_OFF);
  dashState = DashState::Offline;
  requestUpdate();
}

bool PocketDailyActivity::beginSavedWifiConnection(const char* ssid) {
  if (!ssid || !ssid[0]) return false;
  const WifiCredential* cred = WIFI_STORE.findCredential(ssid);
  if (!cred) return false;
  strncpy(joiningSsid, cred->ssid.c_str(), sizeof(joiningSsid) - 1);
  joiningSsid[sizeof(joiningSsid) - 1] = '\0';
  AgentLog::line("AGENT", "background wifi join: %s", joiningSsid);
  WiFi.persistent(false);
  if (cred->password.empty())
    WiFi.begin(cred->ssid.c_str());
  else
    WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
  savedWifiScanActive = false;
  wifiJoinStartMs = millis();
  requestUpdate();
  return true;
}

bool PocketDailyActivity::startSavedWifiJoin(bool pickerOnFailure) {
  // Automatic wake retries stay bounded after one failed scan, but a fresh
  // user Sync is new evidence (the AP may have appeared meanwhile) and must
  // re-scan saved networks before resorting to the picker.
  if (savedWifiJoinFailed && !pickerOnFailure) return false;
  if (pickerOnFailure) savedWifiJoinFailed = false;
  if (WIFI_STORE.getCredentials().empty()) return false;
  // Refuse to raise the radio on a heap that cannot survive the driver's own
  // event traffic. Pocket's CJK card font is the one large block we own and it
  // reloads lazily from SD, so trade it first — exactly as the glance pull
  // does — and only give up if that still leaves us under the floor. An
  // offline Face holding its saved glance is strictly better than the abort()
  // this used to end in (crash_report.txt: NetworkEvents::postEvent one loop
  // after "background wifi scan").
  if (ESP.getMaxAllocHeap() < kWifiBringUpMinBlock || ESP.getFreeHeap() < kWifiBringUpMinFree) {
    // Font families belong to the renderer, so the release must hold its mutex
    // or a queued paint can use the family after it is gone.
    RenderLock fontRenderLock(*this);
    sdFontSystem.releaseLoaded(renderer);
  }
  if (ESP.getMaxAllocHeap() < kWifiBringUpMinBlock || ESP.getFreeHeap() < kWifiBringUpMinFree) {
    if (!wifiHeapBlocked) {
      wifiHeapBlocked = true;
      AgentLog::line("AGENT", "wifi bring-up skipped: heap free=%u largest=%u", (unsigned)ESP.getFreeHeap(),
                     (unsigned)ESP.getMaxAllocHeap());
    }
    savedWifiJoinFailed = true;
    savedWifiScanActive = false;
    dashState = DashState::Offline;
    requestUpdate();
    return false;
  }
  wifiHeapBlocked = false;
  logHeapStage("wifi/pre-radio");
  savedWifiPickerOnFailure = pickerOnFailure;
  joiningSsid[0] = '\0';
  // Scan asynchronously before choosing a credential. Connecting only to the
  // last SSID made a saved home/hotspot set behave as if no credentials
  // existed whenever that one AP was absent. The Face stays fully interactive
  // while scanComplete() is polled from loop().
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);
  WiFi.scanDelete();
  WiFi.scanNetworks(true);
  dashState = DashState::WifiJoining;
  savedWifiScanActive = true;
  wifiJoinStartMs = millis();
  AgentLog::line("AGENT", "background wifi scan: %u saved networks", (unsigned)WIFI_STORE.getCredentials().size());
  requestUpdate();
  return true;
}

void PocketDailyActivity::launchWifiPicker() {
  dashState = DashState::WifiSelection;
  requestUpdate();
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (!result.isCancelled) {
                             const auto& wifi = std::get<WifiResult>(result.data);
                             localIp = wifi.ip;
                           }
                           onWifiSelectionComplete(!result.isCancelled);
                         });
}

void PocketDailyActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    AgentLog::line("POCKET", "wifi selection cancelled — staying offline");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    dashState = DashState::Offline;
    requestUpdate();
    return;
  }
  savedWifiJoinFailed = false;
  if (localIp.empty()) localIp = WiFi.localIP().toString().c_str();
  AgentLog::line("AGENT", "wifi up: %s", localIp.c_str());
  startNetworking();
}

void PocketDailyActivity::logHeapStage(const char* stage) const {
  AgentLog::line("HEAP", "%s: free=%u largest=%u minEver=%u", stage, (unsigned)ESP.getFreeHeap(),
                 (unsigned)ESP.getMaxAllocHeap(), (unsigned)ESP.getMinFreeHeap());
}

void PocketDailyActivity::ensureDiscoveryServices() {
  if (discoveryServicesUp) {
    // udpInit() is idempotent and cheap; WiFi may not have been up when the
    // socket was first bound, so keep letting the Discovering tick retry it.
    AgentDeck::Net::udpInit();
    return;
  }
  discoveryServicesUp = true;
  AgentDeck::Net::mdnsInit(mdnsHostName());
  logHeapStage("disc/mdns");
  AgentDeck::Net::udpInit();
  logHeapStage("disc/udp");
}

void PocketDailyActivity::startNetworking() {
  logHeapStage("net/enter");
  // Best-effort wall clock for saved-deck age and retained-frame sync times.
  // Non-blocking: Pocket remains fully usable while SNTP updates in background.
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  logHeapStage("net/sntp");
  AgentDeck::OtaWs::configureIdentity(gpio.deviceIsX3() ? "xteink_x3" : "xteink_x4");

  // Once paired, Pocket is a short-pull appliance rather than a live
  // dashboard. Reuse the cached endpoint immediately and keep the X3's scarce
  // heap for HTTP/JSON and CJK rendering instead of a permanent WebSocket.
  char endpointIp[16] = {0};
  char endpointToken[40] = {0};
  uint16_t endpointPort = 0;
  if (!pullMode && AgentDeck::AuthStore::load(endpointToken, sizeof(endpointToken)) &&
      AgentDeck::Feed::loadEndpoint(endpointIp, sizeof(endpointIp), endpointPort, endpointToken,
                                    sizeof(endpointToken))) {
    dashState = DashState::Online;
    manualSyncQueued = true;
    requestUpdate();
    return;
  }
  // Only now is discovery genuinely needed, so pay for its services here
  // rather than on every wake.
  ensureDiscoveryServices();
  dashState = DashState::Discovering;
  discoveryStartMs = millis();
  discoveryNoticeShown = false;
  requestUpdate();
}

void PocketDailyActivity::sendClientRegister() {
  // {"type":"client_register","clientType":"eink-device","clientLabel":"XTeink X4",
  //  "devices":[{"id":"<mac>","name":"XTeink X4","family":"xteink-x4","columns":W,"rows":H}]}
  String mac = WiFi.macAddress();
  const char* modelName = deviceModelName();
  const char* modelSlug = deviceModelSlug();
  char clientLabel[48];
  snprintf(clientLabel, sizeof(clientLabel), "%s · %s", PocketDaily::PRODUCT_NAME, modelName);
  char buf[768];
  int n = snprintf(buf, sizeof(buf),
                   "{\"type\":\"client_register\",\"clientType\":\"eink-device\","
                   "\"clientLabel\":\"%s\",\"productId\":\"%s\","
                   "\"surface\":{\"protocol\":%u,\"clientId\":\"%s\",\"clientVersion\":\"%s\","
                   "\"productId\":\"%s\",\"profiles\":[{\"id\":\"%s\",\"capabilities\":%s}]},"
                   "\"devices\":[{\"id\":\"%s\",\"name\":\"%s\",\"family\":\"%s\","
                   "\"columns\":%d,\"rows\":%d}]}",
                   clientLabel, PocketDaily::PRODUCT_ID, PocketDaily::SURFACE_PROTOCOL_REVISION, PocketDaily::CLIENT_ID,
                   CROSSPOINT_VERSION, PocketDaily::PRODUCT_ID, PocketDaily::SURFACE_PROFILE,
                   PocketDaily::SURFACE_CAPABILITIES_JSON, mac.c_str(), modelName, modelSlug, renderer.getScreenWidth(),
                   renderer.getScreenHeight());
  if (n > 0 && (size_t)n < sizeof(buf)) {
    AgentDeck::Net::wsSend(buf);
    AgentLog::line("AGENT", "client_register sent model=%s family=%s mac=%s", modelName, modelSlug, mac.c_str());
  }
  // Ask for initial usage; the daemon pushes state_update/sessions_list on connect.
  AgentDeck::Net::wsSend("{\"type\":\"query_usage\"}");
}

void PocketDailyActivity::sendDeviceInfo() {
  // Announce as a first-class AgentDeck ESP32 device so the daemon registers it
  // in its device registry (Node daemon: type "esp32-wifi"; see AgentDeck
  // docs/esp32-client-contract.md). This is complementary to sendClientRegister():
  // the eink-device roster drives the macOS Dashboard E-ink rail, while device_info
  // drives the ESP32 device registry + OTA identity across all daemons. The board
  // wire string uses the underscore convention (ips_10, ulanzi_tc001, …), distinct
  // from the hyphen family slug in client_register.
  //
  // AgentDeck WiFi OTA v1 supported (src/agentdeck/ota_ws_receiver.*): chunks
  // stream to an SD cache, then flash via the raw-partition path (NOT the
  // Arduino Update class — X4 silicon rejects the patched image through
  // esp_image_verify). Slot capability comes from the live partition table.
  const char* board = gpio.deviceIsX3() ? "xteink_x3" : "xteink_x4";
  String ip = WiFi.localIP().toString();
  uint8_t timelineCount = 0, sessionCount = 0;
  AgentDeck::lockState();
  timelineCount = AgentDeck::g_state.timelineCount;
  sessionCount = AgentDeck::g_state.sessionCount;
  AgentDeck::unlockState();
  const esp_partition_t* otaDest = esp_ota_get_next_update_partition(nullptr);
  const unsigned otaSlotSize = otaDest ? (unsigned)otaDest->size : 0;
  // buildHash = trailing token of CROSSPOINT_VERSION ("1.4.1-dev-master-<sha>").
  const char* buildHash = strrchr(CROSSPOINT_VERSION, '-');
  buildHash = buildHash ? buildHash + 1 : CROSSPOINT_VERSION;
  char buf[640];
  int n = snprintf(buf, sizeof(buf),
                   "{\"type\":\"device_info\",\"productId\":\"%s\",\"productName\":\"%s\","
                   "\"surfaceProfile\":\"%s\",\"surfaceProtocol\":%u,\"sourceProvider\":\"%s\","
                   "\"board\":\"%s\",\"updateChannel\":\"%s\",\"version\":\"%s\",\"buildHash\":\"%s\","
                   "\"protocolRevision\":%u,\"wifiConfigured\":true,\"wifiConnected\":true,"
                   "\"ip\":\"%s\",\"otaSupported\":%s,\"otaSlotCount\":2,\"otaSlotSize\":%u,"
                   "\"otaFreeSketchSpace\":%u,"
                   "\"timelineCount\":%u,\"sessionCount\":%u}",
                   PocketDaily::PRODUCT_ID, PocketDaily::PRODUCT_NAME, PocketDaily::SURFACE_PROFILE,
                   PocketDaily::SURFACE_PROTOCOL_REVISION, PocketDaily::AGENTDECK_PROVIDER_ID, board,
                   PocketDaily::UPDATE_CHANNEL, CROSSPOINT_VERSION, buildHash,
                   (unsigned)AgentDeckCfg::PROTOCOL_REVISION, ip.c_str(), otaDest ? "true" : "false", otaSlotSize,
                   otaSlotSize, (unsigned)timelineCount, (unsigned)sessionCount);
  if (n > 0 && (size_t)n < sizeof(buf)) {
    AgentDeck::Net::wsSend(buf);
    AgentLog::line("AGENT", "device_info sent board=%s ver=%s ip=%s", board, CROSSPOINT_VERSION, ip.c_str());
  }
}

uint32_t PocketDailyActivity::computeStateSignature() const {
  uint32_t h = 2166136261u;
  AgentDeck::lockState();
  const auto& s = AgentDeck::g_state;
  h = fnvUpdate(h, &s.wsConnected, sizeof(s.wsConnected));
  h = fnvUpdate(h, &s.dataReceived, sizeof(s.dataReceived));
  h = fnvUpdate(h, &s.pocketCount, sizeof(s.pocketCount));
  for (uint8_t i = 0; i < s.pocketCount && i < PocketDaily::CARD_CAP; i++) {
    h = fnvUpdate(h, s.pocketCards[i].cardId, strlen(s.pocketCards[i].cardId));
    h = fnvUpdate(h, s.pocketCards[i].question, strlen(s.pocketCards[i].question));
    h = fnvUpdate(h, &s.pocketCards[i].choiceCount, sizeof(s.pocketCards[i].choiceCount));
  }
  h = fnvUpdate(h, &s.glance, sizeof(s.glance));
  AgentDeck::unlockState();
  // Local view/cursor state so navigation repaints.
  uint8_t vm = static_cast<uint8_t>(viewMode);
  h = fnvUpdate(h, &vm, sizeof(vm));
  h = fnvUpdate(h, &overviewCursor, sizeof(overviewCursor));
  h = fnvUpdate(h, &optionCursor, sizeof(optionCursor));
  return h;
}

void PocketDailyActivity::loop() {
  // Abort a stalled OTA receive (sender died mid-push) so `receiving()` can't
  // swallow input forever. Runs before the receive/flash checks below.
  AgentDeck::OtaWs::service();

  // OTA: a fully-received, validated image is waiting. Paint the blocking
  // notice, then flash + restart from this (main) task — never from the WS
  // callback. serviceFlash() only returns on failure.
  if (AgentDeck::OtaWs::flashPending()) {
    otaFlashNotice = true;
    requestUpdateAndWait();
    AgentDeck::OtaWs::serviceFlash();
    otaFlashNotice = false;
    requestUpdate();
    return;
  }
  // Receive-phase progress: repaint the Face status line in 5% steps (e-ink).
  if (AgentDeck::OtaWs::receiving()) {
    const uint32_t total = AgentDeck::OtaWs::totalBytes();
    const int bucket = total ? (int)((uint64_t)AgentDeck::OtaWs::receivedBytes() * 20 / total) : 0;
    if (bucket != otaPctBucket) {
      otaPctBucket = bucket;
      requestUpdate();
    }
  } else {
    otaPctBucket = -1;
  }

  // Heap decay watch. The abort this instruments left a 7-minute telemetry gap
  // between a healthy sync and the fatal scan, so sample slowly and log only a
  // new low-water mark (a full step below the last one). Bounded to a handful
  // of SD lines per boot even on a steadily leaking run.
  if ((int32_t)(millis() - heapWatchNextMs) >= 0) {
    heapWatchNextMs = millis() + heapWatchIntervalMs;
    if (heapWatchIntervalMs < kHeapWatchSlowMs)
      heapWatchIntervalMs = std::min(kHeapWatchSlowMs, heapWatchIntervalMs * 2);
    const uint32_t freeNow = ESP.getFreeHeap();
    if (heapWatchLowFree == 0 || freeNow + kHeapWatchStepBytes <= heapWatchLowFree) {
      heapWatchLowFree = freeNow;
      AgentLog::line("AGENT", "heap low-water: free=%u largest=%u minEver=%u", (unsigned)freeNow,
                     (unsigned)ESP.getMaxAllocHeap(), (unsigned)ESP.getMinFreeHeap());
    }
  }

  // Handle input FIRST so Back stays responsive: the discovery/connect steps
  // below can block (mDNS queryService ~1s, WS connect) and would otherwise
  // starve the button poll, making "go back" feel dead while not yet connected.
  handleButtons();
  if (exitRequested) {
    finish();
    return;
  }
  // Presence cancels the cadence pass: the user pressed something, so this
  // wake continues into the normal interactive WS flow instead of sleeping.
  if (pullMode && lastUserInputMs != 0) {
    AgentLog::line("AGENT", "pull-sync cancelled by input — interactive mode");
    pullMode = false;
    requestUpdate();
  }

  // Deferred connect-edge glance pull, queued by the Connected transition and
  // run here where the call stack is shallowest. Waits out an active OTA
  // receive (a 1-2s blocking fetch mid-stream feeds the rx-stall abort);
  // a dropped link just unqueues — the reconnect edge re-queues it.
  if (glanceRefreshQueued) {
    if (dashState != DashState::Connected) {
      glanceRefreshQueued = false;
    } else if (!AgentDeck::OtaWs::receiving()) {
      glanceRefreshQueued = false;
      const uint32_t maxAgeMs = forceGlanceRefresh ? 0 : 30 * 60 * 1000;
      forceGlanceRefresh = false;
      if (refreshGlanceIfStale(maxAgeMs)) requestUpdate();
      // Diagnosis instruments: stack headroom (the 16c1674b overflow) and
      // heap (the 53a55377 bad_alloc abort — free 7 KB / largest 3.9 KB at
      // power-off). Numbers trending down across a day of logs are the early
      // warning; investigate before either panics again.
      AgentLog::line("AGENT", "post-refresh: stack min-free=%uB heap free=%u largest=%u",
                     (unsigned)uxTaskGetStackHighWaterMark(nullptr), (unsigned)ESP.getFreeHeap(),
                     (unsigned)ESP.getMaxAllocHeap());
    }
  }

  // Front Sync is an explicit pull request, not merely a repaint. Run the
  // HTTP handshake here at the shallowest loop frame. The multi-megabyte OTA
  // body is serviced separately in bounded chunks so input is polled between
  // every network burst.
  if (manualOtaIncrementalActive && !AgentDeck::OtaWs::receiving() && !AgentDeck::OtaWs::flashPending() &&
      (int32_t)(millis() - manualOtaResumeAtMs) >= 0) {
    if (WiFi.status() != WL_CONNECTED) {
      AgentDeck::OtaPull::cancelInteractive();
      manualOtaIncrementalActive = false;
      pullOtaDownloading = false;
      manualOtaResumePending = true;
      manualOtaResumeAtMs = millis() + 2000;
      savedWifiJoinFailed = false;
      startSavedWifiJoin();
      requestUpdate();
    } else {
      // Render and SD-font loading pause only for this small chunk, never for
      // the complete image. On a healthy LAN the lock is held for well under
      // one second; then queued paints and all controls run normally.
      AgentDeck::OtaPull::InteractiveStep step;
      {
        RenderLock chunkLock(*this);
        sdFontSystem.releaseLoaded(renderer);
        step = AgentDeck::OtaPull::serviceInteractive();
      }
      pullOtaDownloadedBytes =
          pullOtaTotalBytes ? AgentDeck::OtaPull::savedBytes(lastManualOtaMd5) : pullOtaDownloadedBytes;
      if (step == AgentDeck::OtaPull::InteractiveStep::Progress) {
        manualOtaNoProgressRetries = 0;
        // Yield one UI loop between complete 128 KiB responses. The old 180 ms
        // pause added several seconds of pure idle time to every image.
        manualOtaResumeAtMs = millis() + 60;
        const int bucket = pullOtaTotalBytes ? (int)((uint64_t)pullOtaDownloadedBytes * 10 / pullOtaTotalBytes) : 0;
        if (bucket != pullOtaPctBucket) {
          pullOtaPctBucket = bucket;
          requestUpdate();
        }
      } else if (step == AgentDeck::OtaPull::InteractiveStep::Retry ||
                 step == AgentDeck::OtaPull::InteractiveStep::Deferred) {
        if (manualOtaNoProgressRetries < 6) manualOtaNoProgressRetries++;
        manualOtaResumeAtMs = millis() + std::min<uint32_t>(15000, 500u << manualOtaNoProgressRetries);
        if (manualOtaNoProgressRetries >= 3) {
          AgentDeck::OtaPull::cancelInteractive();
          manualOtaIncrementalActive = false;
          pullOtaDownloading = false;
          manualSyncNeedsDiscovery = true;
          manualOtaResumePending = true;
          requestUpdate();
        }
      } else if (step == AgentDeck::OtaPull::InteractiveStep::Staged) {
        manualOtaIncrementalActive = false;
        manualOtaResumePending = false;
        pullOtaDownloading = false;
        pullOtaDownloadedBytes = pullOtaTotalBytes;
        pullOtaPctBucket = 10;
        requestUpdate();
      } else if (step == AgentDeck::OtaPull::InteractiveStep::Failed ||
                 step == AgentDeck::OtaPull::InteractiveStep::Idle) {
        AgentDeck::OtaPull::cancelInteractive();
        manualOtaIncrementalActive = false;
        pullOtaDownloading = false;
        manualOtaResumePending = false;
        requestUpdate();
      }
    }
  }

  if (manualOtaResumePending && !manualSyncQueued && !manualSyncActive && !AgentDeck::OtaWs::receiving() &&
      !AgentDeck::OtaWs::flashPending()) {
    const uint32_t now = millis();
    if (manualOtaResumeStartedMs && now - manualOtaResumeStartedMs >= kManualOtaResumeWindowMs) {
      manualOtaResumePending = false;
      pullOtaDownloading = false;
      AgentLog::line("OTA", "automatic resume window spent at %u/%u — keeping SD progress",
                     (unsigned)pullOtaDownloadedBytes, (unsigned)pullOtaTotalBytes);
      requestUpdate();
    } else if ((int32_t)(now - manualOtaResumeAtMs) >= 0) {
      if (WiFi.status() == WL_CONNECTED) {
        if (dashState == DashState::Discovering || dashState == DashState::Connecting) {
          // The normal discovery/connect state machine owns route recovery;
          // do not race it with another request to the stale cached address.
          manualOtaResumeAtMs = now + 1000;
        } else if (manualSyncNeedsDiscovery) {
          manualSyncNeedsDiscovery = false;
          manualSyncDiscoveryRetryActive = true;
          AgentDeck::Net::mdnsRefresh();
          dashState = DashState::Discovering;
          discoveryStartMs = now;
          discoveryNoticeShown = false;
          AgentLog::line("OTA", "automatic resume: refreshing daemon route");
        } else {
          manualSyncQueued = true;
          AgentLog::line("OTA", "automatic resume: retrying at %u/%u", (unsigned)pullOtaDownloadedBytes,
                         (unsigned)pullOtaTotalBytes);
        }
        requestUpdate();
      } else if (dashState != DashState::WifiJoining && dashState != DashState::WifiSelection) {
        // A transfer may drop the STA along with its TCP socket. Rejoin the
        // saved network in the background; never open an interactive picker
        // from an automatic retry.
        savedWifiJoinFailed = false;
        if (startSavedWifiJoin()) {
          manualOtaResumeAtMs = now + 2000;
          AgentLog::line("OTA", "automatic resume: rejoining saved Wi-Fi");
        } else {
          manualOtaResumePending = false;
          AgentLog::line("OTA", "automatic resume paused: %s",
                         wifiHeapBlocked ? "heap too low to raise Wi-Fi" : "no saved Wi-Fi");
          requestUpdate();
        }
      }
    }
  }

  if (manualSyncQueued && !AgentDeck::OtaWs::receiving()) {
    manualSyncQueued = false;
    manualSyncActive = true;
    requestUpdateAndWait();
    logHeapStage("sync/before");
    attemptManualSync();
    logHeapStage("sync/after");
    manualSyncActive = false;
    requestUpdate();
  }

  if (dashState == DashState::WifiJoining) {
    // Background scan/join in progress. The Face and its carousel remain live;
    // only the small status line changes while radio work advances here.
    if (WiFi.status() == WL_CONNECTED) {
      localIp = WiFi.localIP().toString().c_str();
      if (!joiningSsid[0]) {
        strncpy(joiningSsid, WiFi.SSID().c_str(), sizeof(joiningSsid) - 1);
        joiningSsid[sizeof(joiningSsid) - 1] = '\0';
      }
      WIFI_STORE.setLastConnectedSsid(joiningSsid);
      savedWifiJoinFailed = false;
      savedWifiScanActive = false;
      savedWifiPickerOnFailure = false;
      AgentLog::line("AGENT", "wifi joined %s (%s)", joiningSsid, localIp.c_str());
      logHeapStage("wifi/joined");
      startNetworking();
    } else if (savedWifiScanActive) {
      const int16_t scanResult = WiFi.scanComplete();
      if (scanResult == WIFI_SCAN_RUNNING && millis() - wifiJoinStartMs <= kWifiJoinTimeoutMs) return;

      char bestSsid[33] = {0};
      int bestScore = -1000;
      const std::string& lastSsid = WIFI_STORE.getLastConnectedSsid();
      if (scanResult >= 0) {
        for (int i = 0; i < scanResult; i++) {
          const String found = WiFi.SSID(i);
          if (found.isEmpty() || !WIFI_STORE.hasSavedCredential(found.c_str())) continue;
          // Prefer the strongest saved AP, with a small stickiness bonus for
          // the last winner so two similarly strong mesh/home networks do not
          // alternate on every wake.
          const int score = (int)WiFi.RSSI(i) + (lastSsid == found.c_str() ? 12 : 0);
          if (score > bestScore) {
            bestScore = score;
            strncpy(bestSsid, found.c_str(), sizeof(bestSsid) - 1);
          }
        }
      }
      WiFi.scanDelete();
      savedWifiScanActive = false;
      if (bestSsid[0] && beginSavedWifiConnection(bestSsid)) return;

      AgentLog::line("POCKET", "background wifi scan found no saved network");
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      savedWifiJoinFailed = true;
      dashState = DashState::Offline;
      requestUpdate();
      if (!pullMode && savedWifiPickerOnFailure) {
        savedWifiPickerOnFailure = false;
        launchWifiPicker();
      } else if (pullMode) {
        beginTimedSleep(kPullDefaultSec);
      }
    } else if (millis() - wifiJoinStartMs > kWifiJoinTimeoutMs) {
      if (pullMode) {
        // Unattended wake must never end on an interactive picker: give up
        // this cycle and retry on the next timer wake.
        AgentLog::line("AGENT", "wifi join timeout in pull mode — sleeping");
        beginTimedSleep(kPullDefaultSec);
        return;
      }
      AgentLog::line("POCKET", "wifi join timeout (%s) — keeping saved Pocket", joiningSsid);
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      savedWifiJoinFailed = true;
      dashState = DashState::Offline;
      requestUpdate();
      if (savedWifiPickerOnFailure) {
        savedWifiPickerOnFailure = false;
        launchWifiPicker();
      }
    }
    return;
  }

  if (dashState == DashState::Discovering) {
    // Flap cool-down: after kFlapThreshold short-lived connections, stop
    // touching the radio for a while. The Face keeps rendering the last-known
    // deck; the status line says the link is unstable instead of pretending a
    // scan is about to succeed.
    if (nextConnectAllowedMs != 0 && (int32_t)(millis() - nextConnectAllowedMs) < 0) {
      return;
    }
    if (nextConnectAllowedMs != 0) {
      nextConnectAllowedMs = 0;
      flapShortLived = 0;  // fresh chances after the cool-down
      requestUpdate();
    }
    // Every route into Discovering lands here before the first poll, including
    // the two sync-failover paths that jump straight from Online. Idempotent.
    ensureDiscoveryServices();

    // M6 pull mode: cached-endpoint fast path — don't spend the battery window
    // on mDNS when the daemon rarely moves. Failure (daemon restarted onto a
    // different port) falls through to normal discovery below.
    if (pullMode && !pullSynced && !pullEndpointTried) {
      pullEndpointTried = true;
      AgentDeck::Net::EndpointCandidates endpoints;
      char token[40] = {0};
      if (AgentDeck::Feed::loadEndpointCandidates(endpoints, token, sizeof(token))) {
        attemptPullSync(endpoints, token);
      }
    }

    AgentDeck::Net::BridgeInfo bridge;
    // mDNS first (all SRV-target IPv4 addresses, daemon/canonical-port priority),
    // then fall back to the UDP beacon — same BridgeInfo shape, lower trust
    // because anyone on the subnet can broadcast, but the remoteIP/subnet
    // guards in udpPoll() keep it safe.
    bool found = !pullSynced && AgentDeck::Net::mdnsPoll(bridge);
    if (!found && !pullSynced) found = AgentDeck::Net::udpPoll(bridge);
    if (found && bridge.found) {
      if (pullMode) {
        // Pull mode answers discovery with one HTTP sync, not a WS connect.
        attemptPullSync(bridge.endpoints, bridge.token);
      } else {
        char storedToken[40] = {0};
        if (AgentDeck::AuthStore::load(storedToken, sizeof(storedToken))) {
          // Persist the complete service address set before the deferred HTTP
          // pull. If the TXT-preferred interface fails, attemptManualSync()
          // immediately tries the next A record and promotes the winner.
          AgentDeck::Feed::saveEndpointCandidates(bridge.endpoints, storedToken);
          dashState = DashState::Online;
          manualSyncQueued = true;
          discoveryNoticeShown = false;
          requestUpdate();
        } else {
          // The only reason an unpaired Pocket opens a WebSocket is to receive
          // auth_provision from `agentdeck pair --adopt <ip>`.
          AgentLog::line("AGENT", "daemon @ %s:%u (agent=%s) — pairing", bridge.primaryIp(),
                         (unsigned)bridge.endpoints.port, bridge.agent);
          AgentDeck::Net::wsConnect(bridge.primaryIp(), bridge.endpoints.port, bridge.token,
                                    gpio.deviceIsX3() ? "xteink_x3" : "xteink_x4");
          dashState = DashState::Connecting;
          connectStartMs = millis();
          discoveryNoticeShown = false;
          requestUpdate();
        }
      }
    } else if (!discoveryNoticeShown && millis() - discoveryStartMs >= kDiscoveryNotFoundMs) {
      discoveryNoticeShown = true;
      requestUpdate();
    }
  } else if (dashState == DashState::Connecting || dashState == DashState::Connected) {
    AgentDeck::Net::wsLoop();
    AgentDeck::Net::pumpOutbound();

    const bool nowConnected = AgentDeck::Net::wsConnected();
    if (nowConnected && dashState == DashState::Connecting) {
      dashState = DashState::Connected;
      lastConnectedMs = millis();
      // Note: flapShortLived resets only after this connection PROVES healthy
      // (survives kHealthyUptimeMs) — see the drop branch. Resetting here
      // would let a connect-then-die-in-2s loop bypass the cool-down forever.
      if (!registered) {
        sendClientRegister();
        sendDeviceInfo();
        registered = true;
      }
      // Cache the live endpoint for the M6 pull cadence: a later timer wake
      // syncs against it over HTTP without re-running discovery.
      AgentDeck::Feed::saveEndpoint(AgentDeck::Net::wsBridgeIp(), AgentDeck::Net::wsBridgePort(),
                                    AgentDeck::Net::wsBridgeToken());
      // The WS will now stream sessions/usage, but weather and the wrap-up
      // only ride /feed — queue a fetch so the ambient face is complete. NOT
      // inline: the connect pass already sits deep in the loop call chain, and
      // stacking the HTTP+JSON+SD sync chain on top of it overflowed the 8 KB
      // loop task stack (stack-canary panic right after device_info, 3/3).
      // The top of loop() runs it next pass from the shallowest frame.
      glanceRefreshQueued = true;
      requestUpdate();
    } else if (!nowConnected && dashState == DashState::Connecting) {
      char storedToken[40] = {0};
      if (AgentDeck::AuthStore::load(storedToken, sizeof(storedToken))) {
        // auth_provision was persisted and the unauthenticated socket was
        // deliberately closed. Move directly into bounded HTTP sync mode.
        dashState = DashState::Online;
        manualSyncQueued = true;
        registered = false;
        requestUpdate();
        return;
      }
      // The cached ip:port isn't accepting — most likely the daemon moved to a
      // different port (dynamic 9120→fallback). Don't sit on a stale endpoint:
      // after a grace window, re-resolve fresh via mDNS.
      if (millis() - connectStartMs > kConnectTimeoutMs) {
        AgentLog::line("AGENT", "connect timeout — re-resolving via mDNS");
        AgentDeck::Net::wsDisconnect();  // clears saved ip:port, stops stale auto-reconnect
        AgentDeck::Net::mdnsRefresh();   // force an immediate fresh query
        dashState = DashState::Discovering;
        discoveryStartMs = millis();
        discoveryNoticeShown = false;
        registered = false;
        requestUpdate();
      }
    }
    // A connection that survives the healthy window clears the flap ladder.
    if (nowConnected && dashState == DashState::Connected && flapShortLived != 0 &&
        millis() - lastConnectedMs >= kHealthyUptimeMs) {
      flapShortLived = 0;
    }
    if (!nowConnected && dashState == DashState::Connected) {
      const uint32_t uptime = millis() - lastConnectedMs;
      registered = false;
      // Flap ladder: consecutive short-lived connections pause reconnection
      // entirely for a cool-down instead of hammering discovery+connect —
      // the cached Face stays up, which is exactly what it is for.
      if (uptime < kHealthyUptimeMs) {
        flapShortLived++;
        if (flapShortLived >= kFlapThreshold) {
          nextConnectAllowedMs = millis() + kFlapCooldownMs;
          AgentLog::line("AGENT", "link flapping (%d short-lived) — cooling down %us", flapShortLived,
                         (unsigned)(kFlapCooldownMs / 1000));
        }
      }
      if (uptime >= kHealthyUptimeMs) {
        // Was a healthy connection that dropped (transient / daemon restart on the
        // same port): retry the SAME endpoint — the ws_client library auto-reconnects
        // to it. Avoids flapping across multiple daemons on the LAN. If it stays
        // unreachable, the connect-timeout above falls back to a fresh mDNS resolve.
        AgentLog::line("AGENT", "ws dropped after %ums — retrying same endpoint", (unsigned)uptime);
        dashState = DashState::Connecting;
        connectStartMs = millis();
      } else {
        // Endpoint accepted then dropped us quickly — a flaky/duplicate daemon.
        // Re-resolve to try a different advertiser instead of hammering this one.
        AgentLog::line("AGENT", "ws dropped after %ums — re-resolving (flaky endpoint)", (unsigned)uptime);
        AgentDeck::Net::wsDisconnect();
        AgentDeck::Net::mdnsRefresh();
        dashState = DashState::Discovering;
        discoveryStartMs = millis();
        discoveryNoticeShown = false;
      }
      requestUpdate();
    }

    // Repaint only when the rendered state actually changed. Throttled: the loop
    // runs with skipLoopDelay(), so an unthrottled check would re-hash all sessions,
    // timeline entries, and usage strings under g_stateMutex thousands of times per
    // second (pure CPU/power waste + mutex contention with the render task). The
    // interval also coalesces rapid state_update bursts into ≤2 repaints/sec, which
    // is all the e-ink panel can usefully show anyway.
    if (dashState == DashState::Connected && millis() - lastSigCheckMs >= kSigCheckIntervalMs) {
      lastSigCheckMs = millis();
      serviceCard();         // auto-surface / auto-resolve the Decision Card first, so
                             // the signature check below repaints the flip in this tick
      serviceDeckPersist();  // M5.5: keep the SD deck cache in sync (throttled)
      uint32_t sig = computeStateSignature();
      if (sig != lastSignature) {
        lastSignature = sig;
        requestUpdate();
      }
    }
  }

  // SNTP landing upgrades the cached Face's "as of" line from bare to an actual
  // age — repaint once on that transition (it happens at most once per boot).
  const bool nowSynced = time(nullptr) >= 1700000000;
  if (nowSynced != clockSynced) {
    clockSynced = nowSynced;
    requestUpdate();
  }

  // ── M6 power ladder: decide whether this wake goes back to sleep ──
  if (pullMode) {
    servicePullSync();
  } else {
    serviceIdleCadence();
  }
}

bool PocketDailyActivity::refreshGlanceIfStale(uint32_t maxAgeMs) {
  {
    uint32_t at = 0;
    bool valid = false;
    AgentDeck::lockState();
    at = AgentDeck::g_state.glanceAtMs;
    valid = AgentDeck::g_state.glance.valid;
    AgentDeck::unlockState();
    if (valid && at != 0 && millis() - at < maxAgeMs) return false;
  }
  // A Pocket sleep frame is offline-capable. A stale endpoint is not evidence
  // that the radio is usable, so preserve the durable glance without touching
  // DNS/networking when Wi-Fi was never initialized or has disconnected.
  if (!wifiReadyForHttp()) {
    AgentLog::line("AGENT", "glance refresh skipped: Wi-Fi offline");
    return false;
  }
  // Font families belong to the renderer. Hold its mutex for the entire
  // release + HTTP transaction so a queued second paint cannot reload or use
  // the family after requestUpdateAndWait() has returned from an earlier paint.
  RenderLock fontRenderLock(*this);
  // A full feed accumulates into one contiguous string, and esp_http_client
  // itself wants ~5 KB of buffers. On a starved heap the pull cannot succeed
  // — and before the OOM guards landed it aborted the whole device at
  // power-off (X4: free 7 KB / largest 3.9 KB when the feed arrived). An
  // honest stale glance beats a doomed attempt.
  if (ESP.getMaxAllocHeap() < 12 * 1024) {
    // Pocket's CJK card font is the usual owner of the missing contiguous
    // block on X3. It is reloadable from SD after the fetch, so trade that
    // cache for the HTTP/JSON working set before declaring the sync impossible.
    sdFontSystem.releaseLoaded(renderer);
  }
  if (ESP.getMaxAllocHeap() < 12 * 1024) {
    AgentLog::line("AGENT", "glance refresh skipped after font release: heap free=%u largest=%u",
                   (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    return false;
  }
  AgentDeck::Net::EndpointCandidates endpoints;
  char token[40] = {0};
  if (dashState == DashState::Connected && AgentDeck::Net::wsConnected()) {
    endpoints.port = AgentDeck::Net::wsBridgePort();
    AgentDeck::Net::endpointCandidateAdd(endpoints, AgentDeck::Net::wsBridgeIp());
    snprintf(token, sizeof(token), "%s", AgentDeck::Net::wsBridgeToken());
  } else if (!AgentDeck::Feed::loadEndpointCandidates(endpoints, token, sizeof(token))) {
    return false;  // no endpoint known — the cached glance (if any) is all there is
  }
  const char* board = gpio.deviceIsX3() ? "xteink_x3" : "xteink_x4";
  // The no-PSRAM X3 can have less than 3 KB of contiguous heap after the CJK
  // card font is resident. HTTP + JSON needs substantially more, so release
  // the single SD-font slot before pulling. The next render lazily reloads it;
  // an accepted OTA restarts before that is necessary.
  sdFontSystem.releaseLoaded(renderer);
  AgentLog::line("AGENT", "manual sync heap: free=%u largest=%u", (unsigned)ESP.getFreeHeap(),
                 (unsigned)ESP.getMaxAllocHeap());
  AgentDeck::Feed::SyncTelemetry tel;
  tel.battPct = (int)powerManager.getBatteryPercentage();
  tel.rssiDbm = (WiFi.status() == WL_CONNECTED) ? (int)WiFi.RSSI() : 0;
  // The full feed apply also rewrites sessions — same daemon, same roster the
  // WS already delivered, so this is a refresh, not a conflict. On `unchanged`
  // the persisted glance in the deck cache is already current.
  AgentDeck::Feed::SyncResult r;
  for (uint8_t i = 0; i < endpoints.count; i++) {
    r = AgentDeck::Feed::syncOnce(endpoints.ips[i], endpoints.port, token, board, lastFeedSig, tel);
    if (r.ok) break;
    if (i + 1 < endpoints.count)
      AgentLog::line("AGENT", "glance endpoint %s failed — trying %s", endpoints.ips[i], endpoints.ips[i + 1]);
  }
  if (r.ok && !r.unchanged) {
    strncpy(lastFeedSig, r.deckSig, sizeof(lastFeedSig) - 1);
    lastFeedSig[sizeof(lastFeedSig) - 1] = '\0';
    lastDeckSaveMs = 0;
    serviceDeckPersist();
  }
  if (r.ok) {
    // A background refresh is also proof that discovery recovered. Do not
    // leave a stale SYNC FAILED banner after the same route just succeeded.
    manualSyncNeedsDiscovery = false;
    manualSyncDiscoveryRetryActive = false;
  }
  AgentLog::line("AGENT", "glance refresh: %s%s", r.ok ? "ok" : "failed", r.unchanged ? " (unchanged)" : "");
  return r.ok && !r.unchanged;
}

bool PocketDailyActivity::attemptManualSync() {
  if (!wifiReadyForHttp()) {
    manualSyncNeedsDiscovery = true;
    return false;
  }

  AgentDeck::Net::EndpointCandidates endpoints;
  char token[40] = {0};
  if (dashState == DashState::Connected && AgentDeck::Net::wsConnected()) {
    endpoints.port = AgentDeck::Net::wsBridgePort();
    AgentDeck::Net::endpointCandidateAdd(endpoints, AgentDeck::Net::wsBridgeIp());
    snprintf(token, sizeof(token), "%s", AgentDeck::Net::wsBridgeToken());
  } else if (!AgentDeck::Feed::loadEndpointCandidates(endpoints, token, sizeof(token))) {
    AgentLog::line("AGENT", "manual sync: no cached endpoint — refreshing discovery");
    AgentDeck::Net::mdnsRefresh();
    dashState = DashState::Discovering;
    discoveryStartMs = millis();
    discoveryNoticeShown = false;
    return false;
  }

  const char* board = gpio.deviceIsX3() ? "xteink_x3" : "xteink_x4";
  // A deferred paint can still be queued behind requestUpdateAndWait(). Keep
  // every renderer/font access excluded until the HTTP/OTA working set is
  // released; this is the crash barrier for the no-PSRAM X3.
  RenderLock fontRenderLock(*this);
  sdFontSystem.releaseLoaded(renderer);
  AgentDeck::Feed::SyncTelemetry tel;
  tel.battPct = (int)powerManager.getBatteryPercentage();
  tel.rssiDbm = (int)WiFi.RSSI();
  AgentDeck::Feed::SyncResult r;
  char successfulIp[16] = {0};
  for (uint8_t i = 0; i < endpoints.count; i++) {
    r = AgentDeck::Feed::syncOnce(endpoints.ips[i], endpoints.port, token, board, lastFeedSig, tel);
    if (r.ok) {
      snprintf(successfulIp, sizeof(successfulIp), "%s", r.endpointIp[0] ? r.endpointIp : endpoints.ips[i]);
      break;
    }
    if (i + 1 < endpoints.count)
      AgentLog::line("AGENT", "sync endpoint %s failed — trying %s", endpoints.ips[i], endpoints.ips[i + 1]);
  }
  if (!r.ok) {
    AgentLog::line("AGENT", "manual sync failed: %u candidates on port %u", (unsigned)endpoints.count,
                   (unsigned)endpoints.port);
    if (manualOtaResumePending) {
      // A failed resume used to leave resumeAt in the past, causing an
      // mDNS→Feed retry storm every loop (measured: 3.7 MB agentdeck.log and
      // hundreds of doomed sockets). Preserve progress but back off the next
      // route probe; a front-button Sync can still request one immediately.
      if (manualOtaNoProgressRetries < 6) manualOtaNoProgressRetries++;
      const uint32_t retryDelay = std::min<uint32_t>(60000, 1000u << manualOtaNoProgressRetries);
      manualOtaResumeAtMs = millis() + retryDelay;
      AgentLog::line("OTA", "resume feed failed — retry in %ums", (unsigned)retryDelay);
    }
    // One automatic mDNS retry migrates an ADE1 cache to the complete dual-NIC
    // address set. The retry flag makes this bounded: if every freshly
    // discovered candidate also fails, stop on the cached Pocket instead of
    // recreating the old discovery→GET loop.
    if (!manualSyncDiscoveryRetryActive) {
      manualSyncDiscoveryRetryActive = true;
      manualSyncNeedsDiscovery = false;
      AgentDeck::Net::mdnsRefresh();
      dashState = DashState::Discovering;
      discoveryStartMs = millis();
      discoveryNoticeShown = false;
      AgentLog::line("AGENT", "cached endpoints exhausted — one mDNS failover refresh");
    } else {
      manualSyncDiscoveryRetryActive = false;
      manualSyncNeedsDiscovery = true;
      dashState = DashState::Online;
    }
    return false;
  }
  manualSyncNeedsDiscovery = false;
  manualSyncDiscoveryRetryActive = false;
  manualOtaNoProgressRetries = 0;
  // A low battery is only a flash risk when the device is actually running
  // from it. USB power is the safest update posture, so preserve the honest
  // telemetry value in the Feed while bypassing the battery-only OTA gate.
  const int otaBatteryPct = gpio.isUsbConnected() ? -1 : tel.battPct;
  if (r.fwSize) {
    if (AgentDeck::OtaPull::alreadyApplied(r.fwMd5)) {
      manualOtaResumePending = false;
      pullOtaDownloading = false;
      pullOtaDownloadedBytes = 0;
      pullOtaTotalBytes = 0;
    } else {
      const uint32_t before = AgentDeck::OtaPull::savedBytes(r.fwMd5);
      if (!manualOtaResumeStartedMs) manualOtaResumeStartedMs = millis();
      pullOtaTotalBytes = r.fwSize;
      pullOtaDownloadedBytes = before;
      snprintf(lastManualOtaMd5, sizeof(lastManualOtaMd5), "%s", r.fwMd5);
      pullOtaDownloading = true;
      const bool batteryEligible = otaBatteryPct < 0 || otaBatteryPct >= 30;
      if (batteryEligible && AgentDeck::OtaPull::beginInteractive(successfulIp, endpoints.port, token, board, r.fwSize,
                                                                  r.fwMd5, otaBatteryPct)) {
        // Return to loop() immediately. The Face remains interactive while
        // serviceInteractive() advances this MD5-bound SD offset in 128 KiB
        // bursts and repaints only when the visible 10% bucket changes.
        manualOtaIncrementalActive = true;
        manualOtaResumePending = false;
        manualOtaNoProgressRetries = 0;
        manualOtaResumeAtMs = millis();
        pullOtaPctBucket = (int)((uint64_t)before * 10 / r.fwSize);
        AgentLog::line("OTA", "interactive download armed at %u/%u", (unsigned)before, (unsigned)r.fwSize);
      } else {
        pullOtaDownloading = false;
        manualOtaIncrementalActive = false;
        manualOtaResumePending = false;
      }
    }
  }
  if (!r.unchanged) {
    strncpy(lastFeedSig, r.deckSig, sizeof(lastFeedSig) - 1);
    lastFeedSig[sizeof(lastFeedSig) - 1] = '\0';
    lastDeckSaveMs = 0;
    serviceDeckPersist();
  }
  AgentLog::line("AGENT", "manual sync: %s", r.unchanged ? "unchanged" : "updated");
  return true;
}

bool PocketDailyActivity::attemptPullSync(const AgentDeck::Net::EndpointCandidates& endpoints, const char* token) {
  if (!wifiReadyForHttp()) {
    AgentLog::line("AGENT", "pull sync skipped: Wi-Fi offline");
    return false;
  }
  char storedToken[40] = {0};
  const char* resolvedToken = token;
  if ((!resolvedToken || !resolvedToken[0]) && AgentDeck::AuthStore::load(storedToken, sizeof(storedToken))) {
    resolvedToken = storedToken;
  }
  // Keep the complete discovered set before probing it. syncOnce() promotes
  // the winner in this same record, so later timer wakes start with the path
  // that was actually proven while retaining the fallback addresses.
  AgentDeck::Feed::saveEndpointCandidates(endpoints, resolvedToken);
  const char* board = gpio.deviceIsX3() ? "xteink_x3" : "xteink_x4";
  // Timer wakes can overlap the immediate cached-Pocket paint. Serialize font
  // eviction and the HTTP working set with that render just like front Sync.
  RenderLock fontRenderLock(*this);
  sdFontSystem.releaseLoaded(renderer);
  // Telemetry rides the pull — the only battery/link observability a sleeping
  // device has (the daemon logs it per client).
  AgentDeck::Feed::SyncTelemetry tel;
  tel.battPct = (int)powerManager.getBatteryPercentage();
  tel.rssiDbm = (WiFi.status() == WL_CONNECTED) ? (int)WiFi.RSSI() : 0;
  AgentDeck::Feed::SyncResult r;
  char successfulIp[16] = {0};
  for (uint8_t i = 0; i < endpoints.count; i++) {
    r = AgentDeck::Feed::syncOnce(endpoints.ips[i], endpoints.port, resolvedToken, board, lastFeedSig, tel);
    if (r.ok) {
      snprintf(successfulIp, sizeof(successfulIp), "%s", r.endpointIp[0] ? r.endpointIp : endpoints.ips[i]);
      break;
    }
    if (i + 1 < endpoints.count)
      AgentLog::line("AGENT", "pull endpoint %s failed — trying %s", endpoints.ips[i], endpoints.ips[i + 1]);
  }
  if (!r.ok) return false;
  pullSynced = true;
  pullSyncedAtMs = millis();
  pullNextSec = r.nextPullSec;
  // Feed-carried OTA (contract § Pull OTA): a staged build advertised in the
  // feed installs itself on this wake — the flashPending guards in loop() /
  // servicePullSync keep the device awake through download + flash + restart.
  const int otaBatteryPct = gpio.isUsbConnected() ? -1 : tel.battPct;
  if (r.fwSize && !AgentDeck::OtaPull::alreadyApplied(r.fwMd5)) {
    pullOtaTotalBytes = r.fwSize;
    pullOtaDownloadedBytes = AgentDeck::OtaPull::savedBytes(r.fwMd5);
    pullOtaDownloading = true;
    const bool staged = AgentDeck::OtaPull::tryInstall(successfulIp, endpoints.port, resolvedToken, board, r.fwSize,
                                                       r.fwMd5, otaBatteryPct);
    pullOtaDownloadedBytes = staged ? r.fwSize : AgentDeck::OtaPull::savedBytes(r.fwMd5);
    pullOtaDownloading = false;
    if (staged) {
      requestUpdate();
      return true;
    }
    if (pullOtaDownloadedBytes < r.fwSize && (otaBatteryPct < 0 || otaBatteryPct >= 30)) {
      // Unattended cadence is already an automatic retry state machine. A
      // partial image wakes again in five minutes instead of waiting the
      // ordinary hourly content cadence.
      pullNextSec = kPullMinSec;
      AgentLog::line("OTA", "partial cadence image %u/%u — next resume in %us", (unsigned)pullOtaDownloadedBytes,
                     (unsigned)r.fwSize, (unsigned)pullNextSec);
    }
  }
  if (r.unchanged) {
    // Deck unchanged since the last sync: the persisted cache is already
    // exactly what the daemon would have sent. Nothing to parse or persist —
    // this wake's remaining job is repainting the times and sleeping.
    AgentLog::line("AGENT", "pull sync: deck unchanged (sig %s)", lastFeedSig);
    requestUpdate();
    return true;
  }
  strncpy(lastFeedSig, r.deckSig, sizeof(lastFeedSig) - 1);
  lastFeedSig[sizeof(lastFeedSig) - 1] = '\0';
  // Persist immediately (unthrottled): the frozen Face and the wake-time cache
  // must agree on what was just pulled.
  lastDeckSaveMs = 0;
  serviceDeckPersist();
  requestUpdate();
  return true;
}

void PocketDailyActivity::servicePullSync() {
  // Never sleep out from under a firmware transfer.
  if (AgentDeck::OtaWs::receiving() || AgentDeck::OtaWs::flashPending()) return;
  const uint32_t now = millis();
  if (pullSynced) {
    // Linger briefly so a present user can grab the device (any press cancels
    // pull mode above); then freeze the Face and sleep until the next pull.
    if (now - pullSyncedAtMs >= kPullLingerMs) {
      beginTimedSleep(pullNextSec ? pullNextSec : kPullDefaultSec);
    }
    return;
  }
  // Unsynced: Wi-Fi joined but the daemon never answered within the budget —
  // don't burn the battery scanning; retry on the next cadence tick.
  if (now - enterMs >= kPullBudgetMs) {
    AgentLog::line("AGENT", "pull-sync budget exhausted — sleeping unsynced");
    beginTimedSleep(kPullDefaultSec);
  }
}

void PocketDailyActivity::serviceIdleCadence() {
  if (SETTINGS.agentPullSyncEnabled == 0) return;
  if (gpio.isUsbConnected()) return;              // docked → stay in the live WS mode
  if (dashState != DashState::Connected) return;  // pre-connected states keep their own budgets
  if (viewMode != ViewMode::Overview) return;     // never sleep under a Card/Detail
  if (AgentDeck::OtaWs::receiving() || AgentDeck::OtaWs::flashPending()) return;
  // A foreground Sync owns the awake period until it finishes or its bounded
  // retry window expires. Cadence sleep during a resumable transfer was the
  // main reason users saw a fast burst turn into multi-minute gaps.
  if (manualSyncActive || manualOtaIncrementalActive || manualOtaResumePending || pullOtaDownloading) return;
  const uint32_t idleAnchor = lastUserInputMs ? lastUserInputMs : enterMs;
  if (millis() - idleAnchor < kIdleToCadenceMs) return;
  // Agent activity no longer controls this product's power policy. Use the
  // most recent Pocket-feed hint when available, otherwise the hourly default.
  AgentLog::line("POCKET", "idle on battery — entering Pocket cadence");
  beginTimedSleep(pullNextSec ? pullNextSec : kPullDefaultSec);
}

void PocketDailyActivity::beginTimedSleep(uint32_t seconds) {
  if (seconds < kPullMinSec) seconds = kPullMinSec;
  // Idle-cadence entry from WS mode never pulled a feed — fetch the glance
  // (weather / wrap-up) before freezing the frame. The pull-mode path synced
  // seconds ago, so this is a no-op there.
  refreshGlanceIfStale(10 * 60 * 1000);
  // Final deck persist (unthrottled) so the frozen Face and the cache agree.
  lastDeckSaveMs = 0;
  serviceDeckPersist();
  // Absolute wall times for the frozen frame ("Synced HH:MM · next ~HH:MM").
  // Derived from the feed's daemon-local serverHm — the only honest wall clock
  // this device has (no timezone). Empty when no pull anchored it this boot;
  // the glance then falls back to a plain sleep-duration line.
  sleepForSec = seconds;
  sleepNextHm[0] = '\0';
  {
    char baseHm[6] = {0};
    uint32_t baseAtMs = 0;
    AgentDeck::lockState();
    memcpy(baseHm, AgentDeck::g_state.serverHm, sizeof(baseHm));
    baseAtMs = AgentDeck::g_state.serverHmAtMs;
    AgentDeck::unlockState();
    if (baseHm[0])
      AgentDeck::GlanceFormat::addToHm(sleepNextHm, sizeof(sleepNextHm), baseHm,
                                       (millis() - baseAtMs) / 1000UL + seconds);
  }
  // Paint the sleep glance one last time so the retained frame is honest about
  // being a snapshot. The paint serial drives the ghost-clearing FULL_REFRESH.
  bumpTimedSleepPaintSerial();
  glanceReason = GlanceReason::TimedSleep;
  sleepFramePending = true;
  requestUpdateAndWait();
  AgentLog::line("AGENT", "timed deep sleep: %us", (unsigned)seconds);
  enterTimedDeepSleep(seconds, bestEpochNow());
}

void PocketDailyActivity::onExit() {
  Activity::onExit();

  // Cooperative OTA has no background task, so cancellation is immediate and
  // the durable SD offset remains available for the next Pocket Sync.
  AgentDeck::OtaPull::cancelInteractive();
  manualOtaIncrementalActive = false;

  AgentDeck::Net::wsDisconnect();
  AgentDeck::Net::udpStop();
  AgentDeck::Net::mdnsStop();
  MDNS.end();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    // Defrag restart (mirrors CalibreConnectActivity::onExit). Confirm on the
    // glance face targets the reader; every other exit lands on Home.
    if (exitToReader && !APP_STATE.openEpubPath.empty()) {
      silentRestartToReader();
    } else {
      silentRestart();
    }
  }
}

bool PocketDailyActivity::findAwaiting(const char* selected, AwaitingItem& out) const {
  auto cp = [](char* d, size_t n, const char* s) {
    strncpy(d, s, n - 1);
    d[n - 1] = '\0';
  };
  bool found = false;
  AgentDeck::lockState();
  const auto& s = AgentDeck::g_state;
  for (uint8_t i = 0; i < s.sessionCount; i++) {
    const auto& se = s.sessions[i];
    if (strncmp(se.state, "awaiting", 8) != 0 || !selected || strcmp(se.id, selected) != 0) continue;
    cp(out.sid, sizeof(out.sid), se.id);
    cp(out.requestId, sizeof(out.requestId), se.requestId);
    // Options are actionable only when the daemon explicitly identifies this
    // session as their owner. Merely being focused is not enough: aggregate
    // state_update packets can focus an observed session while carrying options
    // left over from another managed PTY.
    const bool optionsCorrelated = s.optionSessionId[0] != '\0' && strcmp(se.id, s.optionSessionId) == 0;
    cp(out.question, sizeof(out.question), optionsCorrelated && s.question[0] ? s.question : se.question);
    out.optionCount = optionsCorrelated ? s.optionCount : 0;
    out.attentionMode = AgentDeck::classifyAttention(true, AgentDeck::isObservedSession(se.controlMode, se.id),
                                                     se.requestId[0] != '\0', optionsCorrelated, out.optionCount);
    found = true;
    break;
  }
  // Focused-state fallback: sessions_list can lag a just-arrived state_update,
  // so accept it only when its session id is the Detail row being inspected.
  if (!found && (s.state == AgentState::AWAITING_PERMISSION || s.state == AgentState::AWAITING_OPTION ||
                 s.state == AgentState::AWAITING_DIFF)) {
    const char* stateSid = s.sessionId[0] ? s.sessionId : s.focusedSessionId;
    if (!selected || strcmp(stateSid, selected) == 0) {
      cp(out.sid, sizeof(out.sid), stateSid);
      cp(out.question, sizeof(out.question), s.question);
      cp(out.requestId, sizeof(out.requestId), s.requestId);
      const bool optionsCorrelated = stateSid[0] && s.optionSessionId[0] && strcmp(stateSid, s.optionSessionId) == 0;
      out.optionCount = optionsCorrelated ? s.optionCount : 0;
      out.attentionMode = AgentDeck::classifyAttention(true, AgentDeck::isObservedSession("", stateSid),
                                                       out.requestId[0] != '\0', optionsCorrelated, out.optionCount);
      found = true;
    }
  }
  AgentDeck::unlockState();
  return found;
}

bool PocketDailyActivity::findPocketCard(const char* cardId, PocketDaily::Card& out) const {
  if (!cardId || !cardId[0]) return false;
  bool found = false;
  AgentDeck::lockState();
  if (localStudyCard.cardId[0] && strcmp(localStudyCard.cardId, cardId) == 0) {
    out = localStudyCard;
    found = true;
  }
  for (uint8_t i = 0; !found && i < AgentDeck::g_state.pocketCount; i++) {
    if (strcmp(AgentDeck::g_state.pocketCards[i].cardId, cardId) != 0) continue;
    out = AgentDeck::g_state.pocketCards[i];
    found = true;
    break;
  }
  if (!found && cachedDeck) {
    for (uint8_t i = 0; i < cachedDeck->pocketCount; i++) {
      if (strcmp(cachedDeck->pocketCards[i].cardId, cardId) != 0) continue;
      out = cachedDeck->pocketCards[i];
      found = true;
      break;
    }
  }
  AgentDeck::unlockState();
  return found;
}

bool PocketDailyActivity::cardUsesSoftkeys(const AgentDeck::AttentionMode mode, const uint8_t optionCount) {
  // Direct button↔choice binding needs every choice on a physical key: slot 1 is
  // always Later, leaving three. Everything else (incl. >3 options) falls back to
  // the cursor grammar inside the card.
  if (mode == AgentDeck::AttentionMode::RealOptions) return optionCount <= 3;
  return mode == AgentDeck::AttentionMode::PermissionGate || mode == AgentDeck::AttentionMode::WaitingForOptions ||
         mode == AgentDeck::AttentionMode::RespondInTerminal;
}

void PocketDailyActivity::serviceCard() {
  if (AgentDeck::OtaWs::receiving() || AgentDeck::OtaWs::flashPending()) return;  // no card takeovers mid-OTA

  if (viewMode == ViewMode::Card) {
    PocketDaily::Card pocket{};
    if (findPocketCard(cardSid, pocket)) {
      // Pocket cards are stable day/info snapshots. They do not auto-resolve
      // with session state; a choice or Later removes them locally.
      return;
    }
    // Live session decisions are intentionally not a Pocket-reader surface.
    // If legacy state left one selected, return to the portable library.
    cardSid[0] = '\0';
    cardSig = 0;
    viewMode = ViewMode::Overview;
    requestUpdate();
    return;
  }
}

int PocketDailyActivity::collectOverview(OverviewRow* out, int cap) const {
  auto cp = [](char* d, size_t n, const char* s) {
    strncpy(d, s, n - 1);
    d[n - 1] = '\0';
  };
  int n = 0;
  auto appendPocket = [&](const PocketDaily::Card& card) {
    if (n >= cap || !card.cardId[0]) return;
    OverviewRow& o = out[n++];
    memset(&o, 0, sizeof(o));
    cp(o.sid, sizeof(o.sid), card.cardId);
    cp(o.project, sizeof(o.project), card.title[0] ? card.title : "POCKET");
    cp(o.agentType, sizeof(o.agentType), "pocket");
    cp(o.controlMode, sizeof(o.controlMode), "pocket");
    cp(o.state, sizeof(o.state), "POCKET");
    cp(o.activity, sizeof(o.activity), card.question);
    if (card.context[0] && strcmp(o.activity, card.context) != 0) {
      const size_t used = strlen(o.activity);
      const size_t extra = (used ? 3 : 0) + strlen(card.context);
      // Keep UTF-8 intact; the detail view still carries the complete context
      // if the compact home row cannot fit it.
      if (used + extra < sizeof(o.activity))
        snprintf(o.activity + used, sizeof(o.activity) - used, "%s%s", used ? " - " : "", card.context);
    }
    o.pocket = true;
  };

  // The first row is always the local book, when one exists. This data lives
  // entirely on SD and remains useful on a device that has never met a daemon.
  if (!APP_STATE.openEpubPath.empty() && n < cap) {
    OverviewRow& o = out[n++];
    memset(&o, 0, sizeof(o));
    cp(o.sid, sizeof(o.sid), "local:reading");
    cp(o.project, sizeof(o.project), tr(STR_POCKET_CONTINUE_READING));
    cp(o.agentType, sizeof(o.agentType), "reader");
    cp(o.state, sizeof(o.state), tr(STR_POCKET_READING));
    const char* title = nullptr;
    const char* author = nullptr;
    for (const auto& book : RECENT_BOOKS.getBooks()) {
      if (book.path == APP_STATE.openEpubPath) {
        title = book.title.c_str();
        author = book.author.c_str();
        break;
      }
    }
    if (!title || !title[0]) {
      const char* path = APP_STATE.openEpubPath.c_str();
      const char* slash = strrchr(path, '/');
      title = slash ? slash + 1 : path;
    }
    snprintf(o.activity, sizeof(o.activity), "%s%s%s", title, author && author[0] ? " - " : "",
             author && author[0] ? author : "");
    o.reading = true;
  }

  // Study is never an empty network placeholder. Put the firmware-authored
  // daily word first, then append any richer daemon cards; side buttons page
  // through the combined portable deck.
  AgentDeck::lockState();
  const PocketDaily::Card local = localStudyCard;
  AgentDeck::unlockState();
  appendPocket(local);

  AgentDeck::lockState();
  const auto& s = AgentDeck::g_state;
  // Daemon-authored Pocket items follow the local book. Live sessions never
  // become top-level rows; they are merely signals the daemon may distil into
  // a portable card.
  for (uint8_t i = 0; i < s.pocketCount && n < cap; i++) {
    appendPocket(s.pocketCards[i]);
  }
  AgentDeck::unlockState();
  return n;
}

uint32_t PocketDailyActivity::bestEpochNow() {
  // NTP system clock first; else the daemon-clock estimate carried by timeline
  // events (works before SNTP lands). 0 = genuinely no clock source this boot.
  const time_t t = time(nullptr);
  if (t >= 1700000000) return (uint32_t)t;
  uint32_t epochSec = 0, epochAtMs = 0;
  AgentDeck::lockState();
  epochSec = AgentDeck::g_state.daemonEpochSec;
  epochAtMs = AgentDeck::g_state.daemonEpochAtMs;
  AgentDeck::unlockState();
  if (epochSec) return epochSec + (millis() - epochAtMs) / 1000UL;
  return 0;
}

void PocketDailyActivity::buildLocalStudyCard() {
  const uint32_t epoch = bestEpochNow();
  const uint32_t day = epoch ? epoch / 86400UL : 0;
  const size_t index = (static_cast<size_t>(day) + localStudyOffset) % kJapaneseDailyWordCount;
  const auto& word = kJapaneseDailyWords[index];

  PocketDaily::Card card{};
  snprintf(card.cardId, sizeof(card.cardId), "local:jp:%lu:%u", (unsigned long)day, (unsigned)localStudyOffset);
  snprintf(card.module, sizeof(card.module), "local");
  snprintf(card.actionClass, sizeof(card.actionClass), "day");
  snprintf(card.title, sizeof(card.title), "今日の単語");
  snprintf(card.question, sizeof(card.question), "%s（%s）", word.word, word.reading);
  snprintf(card.context, sizeof(card.context), "%s · %s", word.meaning, word.example);
  const char* ids[] = {"review", "next", "known"};
  const char* labels[] = {"Again", "Next", "Known"};
  card.choiceCount = 3;
  for (uint8_t i = 0; i < card.choiceCount; i++) {
    snprintf(card.choices[i].id, sizeof(card.choices[i].id), "%s", ids[i]);
    snprintf(card.choices[i].label, sizeof(card.choices[i].label), "%s", labels[i]);
  }

  AgentDeck::lockState();
  localStudyCard = card;
  AgentDeck::unlockState();
  AgentLog::line("POCKET", "offline JP word ready: day=%lu index=%u", (unsigned long)day, (unsigned)index);
}

void PocketDailyActivity::serviceDeckPersist() {
  if (!cachedDeck) return;
  // Never compete with a firmware transfer for SD bandwidth / the WS socket.
  if (AgentDeck::OtaWs::receiving() || AgentDeck::OtaWs::flashPending()) return;
  if (lastDeckSaveMs != 0 && millis() - lastDeckSaveMs < kDeckSaveIntervalMs) return;

  // Content signature of the portable Pocket deck. Live session churn is not
  // product state and must not cause SD writes or e-ink refreshes.
  uint32_t sig = 2166136261u;
  bool dataReceived = false;
  AgentDeck::lockState();
  {
    const auto& s = AgentDeck::g_state;
    dataReceived = s.dataReceived;
    for (uint8_t i = 0; i < s.pocketCount; i++) {
      const auto& card = s.pocketCards[i];
      sig = fnvUpdate(sig, card.cardId, strlen(card.cardId));
      sig = fnvUpdate(sig, card.question, strlen(card.question));
      sig = fnvUpdate(sig, card.context, strlen(card.context));
      sig = fnvUpdate(sig, &card.choiceCount, sizeof(card.choiceCount));
    }
    // The glance block (weather / quota / wrap-up) is part of the persisted
    // snapshot: a weather-only change must still refresh the cache. POD bytes,
    // clear()-normalized, so the raw-memory hash is stable.
    sig = fnvUpdate(sig, reinterpret_cast<const char*>(&s.glance), sizeof(s.glance));
  }
  AgentDeck::unlockState();
  sig = fnvUpdate(sig, lastFeedSig, strlen(lastFeedSig));
  if (!dataReceived) return;  // nothing real to persist yet
  if (sig == lastDeckSig) return;

  // Build the snapshot into the ping-pong partner (never the C3 stack), write it
  // to SD without holding the state lock, then swap it in as the RAM fallback
  // under the lock (the render task reads cachedDeck through g_stateMutex).
  // deckScratch is preallocated in onEnter(); only a boot that was already out
  // of memory falls back to allocating here.
  auto snap = std::move(deckScratch);
  if (!snap) snap = makeUniqueNoThrow<PocketDaily::DeckStore::Snapshot>();
  if (!snap) {
    LOG_ERR("POCKET", "OOM allocating %uB deck snapshot", (unsigned)sizeof(PocketDaily::DeckStore::Snapshot));
    return;
  }
  memset(snap.get(), 0, sizeof(*snap));
  snap->glance.clear();
  strncpy(snap->deckSig, lastFeedSig, sizeof(snap->deckSig) - 1);
  AgentDeck::lockState();
  {
    const auto& s = AgentDeck::g_state;
    snap->glance = s.glance;  // sleep-glance content rides the deck cache (v2)
    snprintf(snap->serverHm, sizeof(snap->serverHm), "%s", s.serverHm);
    snap->pocketCount = s.pocketCount > PocketDaily::CARD_CAP ? PocketDaily::CARD_CAP : s.pocketCount;
    memcpy(snap->pocketCards, s.pocketCards, sizeof(PocketDaily::Card) * snap->pocketCount);
    snap->count = 0;  // legacy session records are never part of Pocket Home
  }
  AgentDeck::unlockState();
  snap->savedEpoch = bestEpochNow();

  lastDeckSaveMs = millis();
  if (!PocketDaily::DeckStore::save(*snap)) {
    deckScratch = std::move(snap);  // SD hiccup — retry on next change, keep the buffer
    return;
  }
  lastDeckSig = sig;
  AgentDeck::lockState();
  cachedDeck.swap(snap);
  AgentDeck::unlockState();
  // snap now holds the superseded cache. Keep it as the next persist's build
  // buffer rather than freeing 6,244 B here and hunting for it again later.
  deckScratch = std::move(snap);
  AgentLog::line("POCKET", "deck persisted: %u items", (unsigned)cachedDeck->pocketCount);
}

void PocketDailyActivity::handleButtons() {
  using Btn = MappedInputManager::Button;

  // Stamp any press: auto-surface must not steal the screen mid-navigation.
  if (mappedInput.wasAnyPressed()) lastUserInputMs = millis();

  // A WiFi OTA transfer rides this activity's WS socket — exiting (or any other
  // action) mid-transfer would tear it down. Swallow input until it resolves.
  if (AgentDeck::OtaWs::receiving() || AgentDeck::OtaWs::flashPending()) return;

  // Pocket Home uses the four physical front positions directly. Card/Detail
  // screens below retain their own explicit grammars.
  if (gpio.wasPressed(HalGPIO::BTN_BACK)) backPressMs = millis();

  // Autonomous Pocket cards remain answerable offline: a press writes the SD
  // outbox first and the next HTTP feed pushes it. This branch intentionally
  // precedes the connection guard below.
  if (viewMode == ViewMode::Card) {
    PocketDaily::Card pocket{};
    if (findPocketCard(cardSid, pocket)) {
      const int raw = mappedInput.getPressedFrontButton();
      if (raw == HalGPIO::BTN_BACK) {
        deferPocketCard(pocket);
      } else {
        int pos = -1;
        if (raw == HalGPIO::BTN_CONFIRM)
          pos = 0;
        else if (raw == HalGPIO::BTN_LEFT)
          pos = 1;
        else if (raw == HalGPIO::BTN_RIGHT)
          pos = 2;
        if (pos >= 0 && pos < pocket.choiceCount) applyPocketChoice(pocket, pos);
      }
      return;
    }
  }

  // Not connected yet (WiFi select / discovering / connecting): Back = leave the
  // dashboard, and when the glance face is up, Confirm = resume the open book.
  // Keep both responsive in every pre-Connected state so the user is never
  // trapped on a "searching…" screen with no way out.
  if (dashState != DashState::Connected) {
    OverviewRow* const rows = inputRows;
    int n = collectOverview(rows, kOverviewCap);
    const int selected = overviewCursor >= 0 && overviewCursor < n ? overviewCursor : 0;

    // Reading and Study are one content carousel. The side keys move through
    // it; Confirm always opens exactly what the panel currently shows.
    if (mappedInput.wasReleased(Btn::Up) && n > 1) {
      overviewCursor = (selected - 1 + n) % n;
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(Btn::Down) && n > 1) {
      overviewCursor = (selected + 1) % n;
      requestUpdate();
      return;
    }
    if (gpio.wasReleased(HalGPIO::BTN_CONFIRM) && n > 0) {
      if (rows[selected].reading) {
        exitToReader = true;
        exitRequested = true;
      } else if (rows[selected].pocket) {
        strncpy(cardSid, rows[selected].sid, sizeof(cardSid) - 1);
        cardSid[sizeof(cardSid) - 1] = '\0';
        viewMode = ViewMode::Card;
        requestUpdate();
      }
      return;
    }
    const bool syncReleased =
        gpio.wasReleased(HalGPIO::BTN_RIGHT) || (gpio.wasReleased(HalGPIO::BTN_CONFIRM) && n == 0);
    if (syncReleased && WiFi.status() != WL_CONNECTED) {
      // A failed HTTP/OTA socket can drop STA while the presentation state is
      // still Online. Sync is a connectivity action, so key it from the radio
      // truth rather than the last painted label; otherwise the button is a
      // silent no-op until the user exits and re-enters Pocket Daily.
      if (dashState != DashState::WifiJoining && dashState != DashState::WifiSelection) {
        // A heap refusal must stay refused — the picker scans too.
        if (!startSavedWifiJoin(true) && !wifiHeapBlocked) launchWifiPicker();
      }
      return;
    }
    if (gpio.wasReleased(HalGPIO::BTN_RIGHT) && WiFi.status() == WL_CONNECTED) {
      if (manualSyncNeedsDiscovery) {
        manualSyncNeedsDiscovery = false;
        manualSyncDiscoveryRetryActive = true;
        AgentDeck::Net::mdnsRefresh();
        dashState = DashState::Discovering;
        discoveryStartMs = millis();
        discoveryNoticeShown = false;
      } else {
        manualSyncDiscoveryRetryActive = false;
        manualOtaResumeStartedMs = millis();
        manualOtaNoProgressRetries = 0;
        manualSyncQueued = true;
      }
      requestUpdate();
      return;
    }
    if (ambientGlanceShown && gpio.wasReleased(HalGPIO::BTN_CONFIRM) && !APP_STATE.openEpubPath.empty()) {
      exitToReader = true;
      exitRequested = true;
      return;
    }
    if (gpio.wasReleased(HalGPIO::BTN_BACK) && backPressMs != 0) exitRequested = true;
    return;
  }

  // ── CARD: full-screen decision — one question, direct physical choices ──
  if (viewMode == ViewMode::Card) {
    AwaitingItem item = {};
    if (!findAwaiting(cardSid, item) || item.attentionMode == AgentDeck::AttentionMode::None) {
      // Resolved between service ticks; serviceCard will repaint — just don't act.
      return;
    }
    const AgentDeck::AttentionMode mode = item.attentionMode;
    const int optCount = (mode == AgentDeck::AttentionMode::PermissionGate) ? 2
                         : (mode == AgentDeck::AttentionMode::RealOptions)  ? item.optionCount
                                                                            : 0;

    // Dismiss ("Later"): remember this prompt's signature so it never re-surfaces
    // unchanged, and clear backPressMs so the release can't read as "exit" from
    // the Overview branch on the next frame.
    const auto dismissCard = [&]() {
      dismissedSigs[dismissedHead] = cardSig;
      dismissedHead = static_cast<uint8_t>((dismissedHead + 1) % kDismissedCap);
      AgentLog::line("AGENT", "card dismissed sid=%s", cardSid);
      cardSid[0] = '\0';
      cardSig = 0;
      viewMode = ViewMode::Overview;
      backPressMs = 0;
      requestUpdate();
    };
    // Open this session's Detail (timeline). The decision stays available there
    // via the inline fallback grammar. Cooldown-stamp so the mapped release of
    // the same physical press can't immediately fire an inline decision.
    const auto openDetail = [&]() {
      strncpy(selectedSid, cardSid, sizeof(selectedSid) - 1);
      selectedSid[sizeof(selectedSid) - 1] = '\0';
      optionCursor = 0;
      detailScroll = 0;
      viewMode = ViewMode::Detail;
      lastDecisionMs = millis();
      backPressMs = 0;
      swallowConfirmRelease = true;
      if (!AgentDeck::isObservedSession("", selectedSid)) AgentDeck::Commands::sendFocusSession(selectedSid);
      AgentDeck::Commands::sendQuerySessionTimeline(selectedSid);
      requestUpdate();
    };

    if (cardUsesSoftkeys(mode, item.optionCount)) {
      // Raw physical order (left→right: BTN_BACK, BTN_CONFIRM, BTN_LEFT,
      // BTN_RIGHT) — renderCard draws hint labels in the same positional order,
      // bypassing the user's logical remap, so display and input always agree.
      const int raw = mappedInput.getPressedFrontButton();
      if (raw == HalGPIO::BTN_BACK) {  // slot 1: Later
        dismissCard();
        return;
      }
      if (mode == AgentDeck::AttentionMode::PermissionGate) {
        if (raw == HalGPIO::BTN_CONFIRM)
          openDetail();  // slot 2: Detail
        else if (raw == HalGPIO::BTN_LEFT)
          applyDecision(item, 1);  // slot 3: Deny
        else if (raw == HalGPIO::BTN_RIGHT)
          applyDecision(item, 0);  // slot 4: Allow
      } else if (mode == AgentDeck::AttentionMode::RealOptions) {
        int pos = -1;
        if (raw == HalGPIO::BTN_CONFIRM)
          pos = 0;  // slot 2: option 1
        else if (raw == HalGPIO::BTN_LEFT)
          pos = 1;  // slot 3: option 2
        else if (raw == HalGPIO::BTN_RIGHT)
          pos = 2;  // slot 4: option 3
        if (pos >= 0 && pos < optCount) applyDecision(item, pos);
      } else {                                          // WaitingForOptions / RespondInTerminal: read-only card
        if (raw == HalGPIO::BTN_CONFIRM) openDetail();  // slot 2: Detail
      }
      return;
    }

    // Cursor grammar (>3 options): side/front navigation moves the highlight,
    // OK selects, Back = Later. Mirrors the Detail inline decision gestures.
    if (mappedInput.wasReleased(Btn::NavNext) && optionCursor < optCount - 1) {
      optionCursor++;
      requestUpdate();
    }
    if (mappedInput.wasReleased(Btn::NavPrevious) && optionCursor > 0) {
      optionCursor--;
      requestUpdate();
    }
    if (mappedInput.wasReleased(Btn::Confirm) && optCount > 0) {
      applyDecision(item, optionCursor);
      requestUpdate();
    }
    if (mappedInput.wasReleased(Btn::Back) && backPressMs != 0) dismissCard();
    return;
  }

  // ── DETAIL: session timeline + (when awaiting) the decision options inline ──
  if (viewMode == ViewMode::Detail) {
    // Resolve the attention contract for this session. Only a requestId gate or
    // explicitly-correlated real options is actionable; observed terminal-only
    // prompts never acquire synthetic buttons.
    AwaitingItem item = {};
    const bool awaiting = findAwaiting(selectedSid, item);
    AgentDeck::AttentionMode attentionMode = AgentDeck::AttentionMode::None;
    int optCount = 0;
    if (awaiting) {
      attentionMode = item.attentionMode;
      if (attentionMode == AgentDeck::AttentionMode::PermissionGate)
        optCount = 2;  // real PreToolUse gate: Allow / Deny
      else if (attentionMode == AgentDeck::AttentionMode::RealOptions)
        optCount = item.optionCount;
    }
    if (optCount > 0) {
      if (optionCursor >= optCount) optionCursor = optCount - 1;
      if (optionCursor < 0) optionCursor = 0;
    }
    const bool atBottom = (detailScroll >= detailMaxScroll);

    // Up/Down: scroll the content; once at the bottom (options in view) the same
    // buttons move the option highlight, so it's one continuous gesture.
    if (mappedInput.wasReleased(Btn::NavNext)) {  // Down
      if (awaiting && atBottom && optionCursor < optCount - 1)
        optionCursor++;
      else if (detailScroll < detailMaxScroll)
        detailScroll++;
      requestUpdate();
    }
    if (mappedInput.wasReleased(Btn::NavPrevious)) {  // Up
      if (awaiting && atBottom && optionCursor > 0)
        optionCursor--;
      else if (detailScroll > 0)
        detailScroll--;
      requestUpdate();
    }

    // OK confirms the highlighted option once the decision is in view.
    if (swallowConfirmRelease && mappedInput.wasReleased(Btn::Confirm)) {
      // The release of the raw press that opened Detail from the Card — inert.
      swallowConfirmRelease = false;
    } else if (mappedInput.wasReleased(Btn::Confirm) && AgentDeck::attentionIsActionable(attentionMode) &&
               optCount > 0 && atBottom) {
      applyDecision(item, optionCursor);
      // Stay in Detail until the daemon confirms the state transition. This
      // avoids making a dropped/no-op command look successful on slow e-ink.
      requestUpdate();
    }

    if (mappedInput.wasReleased(Btn::Back)) {
      viewMode = ViewMode::Overview;
      requestUpdate();
    }
    return;
  }

  // ── OVERVIEW: one Reading/Study carousel + explicit Library/Sync ──
  OverviewRow* const rows = inputRows;
  const int n = collectOverview(rows, kOverviewCap);
  const int selected = overviewCursor >= 0 && overviewCursor < n ? overviewCursor : 0;

  if (mappedInput.wasReleased(Btn::Up) && n > 1) {
    overviewCursor = (selected - 1 + n) % n;
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(Btn::Down) && n > 1) {
    overviewCursor = (selected + 1) % n;
    requestUpdate();
    return;
  }

  if (gpio.wasReleased(HalGPIO::BTN_CONFIRM) && n > 0) {
    if (rows[selected].reading) {
      exitToReader = true;
      exitRequested = true;
    } else if (rows[selected].pocket) {
      strncpy(cardSid, rows[selected].sid, sizeof(cardSid) - 1);
      cardSid[sizeof(cardSid) - 1] = '\0';
      optionCursor = 0;
      viewMode = ViewMode::Card;
      requestUpdate();
    }
    return;
  }
  if (gpio.wasReleased(HalGPIO::BTN_RIGHT) || (gpio.wasReleased(HalGPIO::BTN_CONFIRM) && n == 0)) {
    if (manualSyncNeedsDiscovery) {
      manualSyncNeedsDiscovery = false;
      AgentDeck::Net::mdnsRefresh();
      dashState = DashState::Discovering;
      discoveryStartMs = millis();
      discoveryNoticeShown = false;
    } else {
      manualOtaResumeStartedMs = millis();
      manualOtaNoProgressRetries = 0;
      manualSyncQueued = true;
    }
    requestUpdate();
    return;
  }

  // Back exits the dashboard (guard a stale release from a prior activity).
  if (gpio.wasReleased(HalGPIO::BTN_BACK) && backPressMs != 0) exitRequested = true;
}

bool PocketDailyActivity::applyDecision(const AwaitingItem& it, int selectedCursor) {
  if (millis() - lastDecisionMs < kDecisionCooldownMs) return false;

  if (it.attentionMode == AgentDeck::AttentionMode::PermissionGate) {
    if (!it.requestId[0] || (selectedCursor != 0 && selectedCursor != 1)) return false;
    AgentDeck::Commands::sendPermissionDecision(it.requestId, selectedCursor == 0 ? "allow" : "deny");
    AgentLog::line("AGENT", "permission_decision=%s sid=%s req=%s", selectedCursor == 0 ? "allow" : "deny", it.sid,
                   it.requestId);
  } else if (it.attentionMode == AgentDeck::AttentionMode::RealOptions) {
    int optionIndex = -1;
    bool navigable = false;
    char action[40] = {0};
    AgentDeck::lockState();
    const auto& s = AgentDeck::g_state;
    const bool stillCorrelated = it.sid[0] && s.optionSessionId[0] && strcmp(it.sid, s.optionSessionId) == 0;
    if (stillCorrelated && selectedCursor >= 0 && selectedCursor < s.optionCount) {
      optionIndex = s.options[selectedCursor].index;
      navigable = s.navigable;
      strncpy(action, s.options[selectedCursor].action, sizeof(action) - 1);
    }
    AgentDeck::unlockState();
    if (optionIndex < 0) return false;
    if (navigable) {
      AgentDeck::Commands::sendSelectOption(it.sid, optionIndex);
      AgentLog::line("AGENT", "select_option idx=%d sid=%s", optionIndex, it.sid);
    } else {
      if (!action[0]) return false;
      AgentDeck::Commands::sendRespond(action);
      AgentLog::line("AGENT", "respond shortcut=%s sid=%s", action, it.sid);
    }
  } else {
    return false;
  }

  lastDecisionMs = millis();
  requestUpdate();
  return true;
}

void PocketDailyActivity::dismissPocketCard(const char* cardId) {
  if (!cardId || !cardId[0]) return;
  bool cachedCardRemoved = false;
  bool liveDeckAvailable = false;
  AgentDeck::lockState();
  auto& state = AgentDeck::g_state;
  for (uint8_t i = 0; i < state.pocketCount; i++) {
    if (strcmp(state.pocketCards[i].cardId, cardId) != 0) continue;
    if (i + 1 < state.pocketCount)
      memmove(&state.pocketCards[i], &state.pocketCards[i + 1],
              sizeof(PocketDaily::Card) * (state.pocketCount - i - 1));
    state.pocketCount--;
    memset(&state.pocketCards[state.pocketCount], 0, sizeof(PocketDaily::Card));
    break;
  }
  liveDeckAvailable = state.dataReceived;
  if (cachedDeck) {
    for (uint8_t i = 0; i < cachedDeck->pocketCount; i++) {
      if (strcmp(cachedDeck->pocketCards[i].cardId, cardId) != 0) continue;
      if (i + 1 < cachedDeck->pocketCount)
        memmove(&cachedDeck->pocketCards[i], &cachedDeck->pocketCards[i + 1],
                sizeof(PocketDaily::Card) * (cachedDeck->pocketCount - i - 1));
      cachedDeck->pocketCount--;
      memset(&cachedDeck->pocketCards[cachedDeck->pocketCount], 0, sizeof(PocketDaily::Card));
      cachedCardRemoved = true;
      break;
    }
  }
  AgentDeck::unlockState();

  // Pocket choices are deliberately usable without a daemon. Persist the
  // local removal immediately as well as the outbox decision; otherwise a
  // reboot before the next successful feed would resurrect the cached card and
  // allow the same choice to be queued twice. Keep the original savedEpoch:
  // removing a card locally does not make the rest of the snapshot fresher.
  if (liveDeckAvailable) {
    // A freshly-received card may not exist in cachedDeck yet. Bypass the
    // normal five-second throttle and snapshot the now-updated live deck.
    lastDeckSaveMs = 0;
    serviceDeckPersist();
  } else if (cachedDeck && cachedCardRemoved && !PocketDaily::DeckStore::save(*cachedDeck)) {
    AgentLog::line("POCKET", "card cache removal not persisted: %s", cardId);
  }
  AgentLog::line("POCKET", "card hidden: %s", cardId);
  cardSid[0] = '\0';
  cardSig = 0;
  viewMode = ViewMode::Overview;
  backPressMs = 0;
  requestUpdate();
}

bool PocketDailyActivity::deferPocketCard(const PocketDaily::Card& card) {
  if (millis() - lastDecisionMs < kDecisionCooldownMs || !card.cardId[0]) return false;
  if (strcmp(card.module, "local") == 0) {
    // Back/Later on the built-in lesson simply closes it. It is not a daemon
    // decision and must not poison the offline outbox with an unknown module.
    lastDecisionMs = millis();
    cardSid[0] = '\0';
    viewMode = ViewMode::Overview;
    backPressMs = 0;
    requestUpdate();
    return true;
  }
  PocketDaily::OutboxStore::Record rec{};
  strncpy(rec.cardId, card.cardId, sizeof(rec.cardId) - 1);
  strncpy(rec.action, "card_choice", sizeof(rec.action) - 1);
  strncpy(rec.choiceId, "later", sizeof(rec.choiceId) - 1);
  rec.index = -1;
  rec.recordedEpoch = bestEpochNow();
  if (!PocketDaily::OutboxStore::append(rec)) {
    AgentLog::line("POCKET", "Later not queued: %s", card.cardId);
    return false;
  }
  lastDecisionMs = millis();
  dismissPocketCard(card.cardId);
  glanceRefreshQueued = true;
  return true;
}

bool PocketDailyActivity::applyPocketChoice(const PocketDaily::Card& card, int selectedCursor) {
  if (millis() - lastDecisionMs < kDecisionCooldownMs || selectedCursor < 0 || selectedCursor >= card.choiceCount)
    return false;
  const auto& choice = card.choices[selectedCursor];
  if (!card.cardId[0] || !choice.id[0]) return false;
  if (strcmp(card.module, "local") == 0) {
    // Again keeps today's word; Next and Known move through the offline starter
    // deck immediately. A future spaced-repetition store can refine this without
    // changing the card/button contract.
    if (strcmp(choice.id, "next") == 0 || strcmp(choice.id, "known") == 0) {
      localStudyOffset = static_cast<uint8_t>((localStudyOffset + 1) % kJapaneseDailyWordCount);
      buildLocalStudyCard();
    }
    lastDecisionMs = millis();
    cardSid[0] = '\0';
    viewMode = ViewMode::Overview;
    backPressMs = 0;
    requestUpdate();
    return true;
  }
  PocketDaily::OutboxStore::Record rec{};
  strncpy(rec.cardId, card.cardId, sizeof(rec.cardId) - 1);
  strncpy(rec.action, "card_choice", sizeof(rec.action) - 1);
  strncpy(rec.choiceId, choice.id, sizeof(rec.choiceId) - 1);
  rec.index = -1;
  rec.recordedEpoch = bestEpochNow();
  if (!PocketDaily::OutboxStore::append(rec)) {
    AgentLog::line("POCKET", "choice not queued: %s", card.cardId);
    return false;
  }
  AgentLog::line("POCKET", "choice queued: %s=%s", card.cardId, choice.id);
  lastDecisionMs = millis();
  dismissPocketCard(card.cardId);
  // The next shallow loop pass pushes the outbox before fetching a replacement
  // feed. Never run the deep HTTP/JSON/SD chain from this button stack.
  glanceRefreshQueued = true;
  return true;
}

int PocketDailyActivity::fontForText(int uiFontId, const char* text) const {
  return UiCjkFont::fontForText(renderer, text, uiFontId, EpdFontFamily::REGULAR,
                                UiCjkFont::CoveragePolicy::RequireFull);
}

void PocketDailyActivity::preparePersonalSnapshot() {
  renderGlanceSnapshot.clear();
  renderSyncedHm[0] = '\0';
  renderSavedEpoch = 0;
  memset(&renderPocketSnapshot, 0, sizeof(renderPocketSnapshot));

  AgentDeck::lockState();
  if (AgentDeck::g_state.glance.valid)
    renderGlanceSnapshot = AgentDeck::g_state.glance;
  else if (cachedDeck)
    renderGlanceSnapshot = cachedDeck->glance;

  if (AgentDeck::g_state.serverHm[0])
    snprintf(renderSyncedHm, sizeof(renderSyncedHm), "%s", AgentDeck::g_state.serverHm);
  else if (cachedDeck && cachedDeck->serverHm[0])
    snprintf(renderSyncedHm, sizeof(renderSyncedHm), "%s", cachedDeck->serverHm);
  if (cachedDeck) renderSavedEpoch = cachedDeck->savedEpoch;

  if (localStudyCard.cardId[0])
    renderPocketSnapshot = localStudyCard;
  else if (AgentDeck::g_state.pocketCount > 0)
    renderPocketSnapshot = AgentDeck::g_state.pocketCards[0];
  else if (cachedDeck && cachedDeck->pocketCount > 0)
    renderPocketSnapshot = cachedDeck->pocketCards[0];
  AgentDeck::unlockState();

  renderReadingSnapshot.clear();
  if (APP_STATE.openEpubPath.empty()) return;

  renderReadingSnapshot.valid = true;
  for (const auto& book : RECENT_BOOKS.getBooks()) {
    if (book.path != APP_STATE.openEpubPath) continue;
    snprintf(renderReadingSnapshot.title, sizeof(renderReadingSnapshot.title), "%s", book.title.c_str());
    snprintf(renderReadingSnapshot.author, sizeof(renderReadingSnapshot.author), "%s", book.author.c_str());
    if (!book.coverBmpPath.empty()) {
      // Pocket's editorial hero needs the full retained cover. The previous
      // code always selected Home's small thumbnail; drawBitmap deliberately
      // does not upscale, so additional layout space could never enlarge it.
      std::string fullPath = book.coverBmpPath;
      const size_t thumb = fullPath.rfind("/thumb_");
      if (thumb != std::string::npos) fullPath.replace(thumb, std::string::npos, "/cover.bmp");
      const std::string thumbPath =
          UITheme::getCoverThumbPath(book.coverBmpPath, UITheme::getInstance().getMetrics().homeCoverHeight);
      const std::string& displayPath = Storage.exists(fullPath.c_str()) ? fullPath : thumbPath;
      snprintf(renderReadingSnapshot.coverBmpPath, sizeof(renderReadingSnapshot.coverBmpPath), "%s",
               displayPath.c_str());
    }
    break;
  }
  if (!renderReadingSnapshot.title[0]) {
    const char* path = APP_STATE.openEpubPath.c_str();
    const char* slash = strrchr(path, '/');
    snprintf(renderReadingSnapshot.title, sizeof(renderReadingSnapshot.title), "%s", slash ? slash + 1 : path);
  }

  // The seventh progress byte is a backwards-compatible whole-book percent.
  // Read it once per paint through HalStorage; older six-byte files simply keep
  // percent=-1 and still resume at their exact local spine/page position.
  if (!FsHelpers::hasEpubExtension(APP_STATE.openEpubPath)) return;
  char progressPath[64];
  snprintf(progressPath, sizeof(progressPath), "/.crosspoint/epub_%u/progress.bin",
           (unsigned)std::hash<std::string>{}(APP_STATE.openEpubPath));
  HalFile progressFile;
  if (!Storage.openFileForRead("POCKET", progressPath, progressFile)) return;
  uint8_t progressData[7];
  if (progressFile.read(progressData, sizeof(progressData)) == (int)sizeof(progressData) && progressData[6] <= 100)
    renderReadingSnapshot.percent = (int8_t)progressData[6];
}

bool PocketDailyActivity::drawReadingCover(int x, int y, int width, int height) const {
  if (width <= 12 || height <= 18) return false;
  bool coverDrawn = false;
  if (renderReadingSnapshot.coverBmpPath[0]) {
    HalFile coverFile;
    if (Storage.openFileForRead("POCKET", renderReadingSnapshot.coverBmpPath, coverFile)) {
      Bitmap bitmap(coverFile);
      if (bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() > 0 && bitmap.getHeight() > 0) {
        // Home thumbnails are intentionally small. Pocket's editorial hero is
        // allowed to scale that retained 1-bit bitmap up to the available
        // frame; capping at 1.0 left a tiny image floating inside a large box.
        const float scale = std::min((float)(width - 4) / bitmap.getWidth(), (float)(height - 4) / bitmap.getHeight());
        const int drawW = std::max(1, (int)(bitmap.getWidth() * scale));
        const int drawH = std::max(1, (int)(bitmap.getHeight() * scale));
        renderer.drawBitmap(bitmap, x + (width - drawW) / 2, y + (height - drawH) / 2, drawW, drawH, 0, 0, true);
        coverDrawn = true;
      }
      coverFile.close();
    }
  }

  renderer.drawRect(x, y, width, height, 2, true);
  if (!coverDrawn) {
    // Private/no-art fallback: still reads as a book, but never invents a
    // remote cover or leaks title text when sleep-cover privacy is disabled.
    renderer.drawLine(x + 8, y, x + 8, y + height, true);
    const int horizon = y + height * 61 / 100;
    renderer.drawLine(x + 10, horizon, x + width - 2, horizon, true);
    const int mountainX[] = {x + 10, x + width / 3, x + width / 2, x + width * 3 / 4, x + width - 2};
    const int mountainY[] = {horizon, y + height * 41 / 100, horizon - 4, y + height * 31 / 100, horizon};
    for (int i = 0; i < 4; i++) renderer.drawLine(mountainX[i], mountainY[i], mountainX[i + 1], mountainY[i + 1], true);
    for (int i = 0; i < 4; i++) {
      const int tx = x + 18 + i * std::max(8, (width - 32) / 4);
      renderer.drawLine(tx, y + height - 8, tx + 5, horizon - 2, 2, true);
      renderer.drawLine(tx + 10, y + height - 8, tx + 5, horizon - 2, 2, true);
    }
  }
  return coverDrawn;
}

void PocketDailyActivity::drawBrandedHeader(const char* title, const char* subtitle) const {
  const auto& m = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();
  const Rect r{0, m.topPadding, w, m.headerHeight};
  // Product identity is Pocket itself. AgentDeck is an invisible sync source,
  // so its logo and wordmark never appear in the reader shell.
  GUI.drawHeader(renderer, r, title, subtitle);
}

void PocketDailyActivity::renderOverview(const OverviewRow* rows, int n, int awaitingCount, bool fromCache,
                                         uint32_t asOfEpoch) {
  const auto& m = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();
  const bool sidePaging = n > 1;
  // X3 gets borderless edge chevrons instead of the old 30px PREV/NEXT rails.
  // X4 has enough width to retain its hardware-aligned side hints.
  const int pad = sidePaging && !gpio.deviceIsX3() ? std::max(m.contentSidePadding, m.sideButtonHintsWidth + 6)
                                                   : m.contentSidePadding;
  const int line12 = renderer.getLineHeight(UI_12_FONT_ID);
  const int line10 = renderer.getLineHeight(UI_10_FONT_ID);
  (void)awaitingCount;  // live Agent attention is not a Pocket Home concern

  preparePersonalSnapshot();
  renderer.clearScreen();
  char statusLine[96];
  if (AgentDeck::OtaWs::receiving()) {
    const uint32_t total = AgentDeck::OtaWs::totalBytes();
    const unsigned pct = total ? (unsigned)((uint64_t)AgentDeck::OtaWs::receivedBytes() * 100 / total) : 0;
    snprintf(statusLine, sizeof(statusLine), "%s \xC2\xB7 %u%%", tr(STR_POCKET_FIRMWARE), pct);
  } else if (pullOtaTotalBytes > 0 && pullOtaDownloadedBytes < pullOtaTotalBytes) {
    const unsigned pct = (unsigned)((uint64_t)pullOtaDownloadedBytes * 100 / pullOtaTotalBytes);
    if (pullOtaDownloading || manualSyncActive)
      snprintf(statusLine, sizeof(statusLine), "%s \xC2\xB7 %u%% \xC2\xB7 DOWNLOADING", tr(STR_POCKET_FIRMWARE), pct);
    else if (manualOtaResumePending)
      snprintf(statusLine, sizeof(statusLine), "%s \xC2\xB7 %u%% \xC2\xB7 AUTO RESUME", tr(STR_POCKET_FIRMWARE), pct);
    else
      snprintf(statusLine, sizeof(statusLine), "%s \xC2\xB7 %u%% \xC2\xB7 SYNC TO RESUME", tr(STR_POCKET_FIRMWARE),
               pct);
  } else if (manualSyncQueued || manualSyncActive) {
    snprintf(statusLine, sizeof(statusLine), "%s", tr(STR_POCKET_CHECKING));
  } else if (manualSyncNeedsDiscovery) {
    snprintf(statusLine, sizeof(statusLine), "SYNC FAILED / RETRY");
  } else if (pullMode && pullSynced) {
    snprintf(statusLine, sizeof(statusLine), "%s \xC2\xB7 %s", tr(STR_POCKET_UPDATED), tr(STR_POCKET_SLEEPING));
  } else if (pullMode) {
    snprintf(statusLine, sizeof(statusLine), "%s", tr(STR_POCKET_CHECKING));
  } else {
    // Link truth comes from the radio itself. dashState describes what Pocket
    // is trying to do and can lag a disconnect/reconnect edge by one loop, so
    // it must never be the sole source of an ONLINE label.
    const bool wifiUp = WiFi.status() == WL_CONNECTED;
    if (dashState == DashState::WifiSelection) {
      snprintf(statusLine, sizeof(statusLine), "WI-FI SETUP");
    } else if (!wifiUp && dashState == DashState::WifiJoining) {
      if (savedWifiScanActive)
        snprintf(statusLine, sizeof(statusLine), "WI-FI SEARCHING / SAVED NETWORKS");
      else
        snprintf(statusLine, sizeof(statusLine), "WI-FI JOINING%s%s", joiningSsid[0] ? " / " : "", joiningSsid);
    } else if (!wifiUp) {
      snprintf(statusLine, sizeof(statusLine), "WI-FI OFF / SYNC");
    } else if ((dashState == DashState::Connected && AgentDeck::Net::wsConnected()) || dashState == DashState::Online) {
      const String currentIp = WiFi.localIP().toString();
      snprintf(statusLine, sizeof(statusLine), "ONLINE / %s", currentIp.c_str());
    } else if (dashState == DashState::Connecting) {
      snprintf(statusLine, sizeof(statusLine), "WI-FI OK / DECK CONNECT");
    } else {
      snprintf(statusLine, sizeof(statusLine), "WI-FI OK / DECK SEARCH");
    }
  }
  if (fromCache) {
    char savedStatus[40];
    const time_t nowT = time(nullptr);
    if (asOfEpoch && nowT >= 1700000000 && (uint32_t)nowT >= asOfEpoch) {
      const uint32_t age = (uint32_t)nowT - asOfEpoch;
      if (age >= 60) {
        char a[8];
        formatAge(age, a, sizeof(a));
        snprintf(savedStatus, sizeof(savedStatus), "%s \xC2\xB7 %s", tr(STR_POCKET_SAVED), a);
      } else {
        snprintf(savedStatus, sizeof(savedStatus), "%s", tr(STR_POCKET_SAVED));
      }
    } else {
      snprintf(savedStatus, sizeof(savedStatus), "%s", tr(STR_POCKET_SAVED));
    }
    // Keep connectivity first: a saved deck must never make an offline device
    // look online. The second phrase explains why its content is still usable.
    char linkStatus[sizeof(statusLine)];
    snprintf(linkStatus, sizeof(linkStatus), "%s", statusLine);
    snprintf(statusLine, sizeof(statusLine), "%s \xC2\xB7 %s", linkStatus, savedStatus);
  }
  // Network truth gets its own bounded row. Theme headers were designed for a
  // short subtitle; long Wi-Fi/Deck/Saved combinations could run into battery
  // or control chrome and became unreadable on X3.
  // The active Overview already has a dedicated network/status band and the
  // weather panel owns its snapshot date. Repeating date + sync time in the
  // compact header collides with the hero copy on X3 and visually crowds the
  // physical Sync key, so keep this header product-only. Retained sleep Glance
  // continues to show its immutable snapshot metadata separately.
  drawBrandedHeader(tr(STR_POCKET_DAILY), nullptr);
  const int statusBandY = m.topPadding + m.headerHeight;
  const int statusBandH = renderer.getLineHeight(SMALL_FONT_ID) + 4;
  const bool syncInk = savedWifiScanActive || manualSyncQueued || manualSyncActive || manualOtaIncrementalActive ||
                       pullOtaDownloading || AgentDeck::OtaWs::receiving();
  if (syncInk) {
    // E-ink activity signal: invert the quiet status row, then advance four
    // white toner blocks only when the phase/progress changes. It reads as
    // active without a fake spinner or repeated refresh animation.
    renderer.fillRect(pad, statusBandY, w - pad * 2, statusBandH, true);
    const int cellsW = 52;
    const int cellGap = 3;
    const int cellW = (cellsW - cellGap * 3) / 4;
    int activeCells = savedWifiScanActive ? 1 : (manualSyncQueued || manualSyncActive ? 2 : 1);
    if (pullOtaTotalBytes && pullOtaDownloadedBytes < pullOtaTotalBytes)
      activeCells = std::max(
          1, std::min(4, (int)(((uint64_t)pullOtaDownloadedBytes * 4 + pullOtaTotalBytes - 1) / pullOtaTotalBytes)));
    const int cellsX = w - pad - cellsW - 7;
    for (int i = 0; i < 4; i++) {
      const int cellX = cellsX + i * (cellW + cellGap);
      if (i < activeCells)
        renderer.fillRect(cellX, statusBandY + 4, cellW, statusBandH - 8, false);
      else
        renderer.drawRect(cellX, statusBandY + 4, cellW, statusBandH - 8, false);
    }
    renderer.drawText(SMALL_FONT_ID, pad + 8, statusBandY + 1,
                      renderer.truncatedText(SMALL_FONT_ID, statusLine, cellsX - pad - 14).c_str(), false,
                      EpdFontFamily::BOLD);
  } else {
    renderer.fillRect(pad, statusBandY + 3, 3, std::max(4, statusBandH - 7), true);
    renderer.drawText(SMALL_FONT_ID, pad + 10, statusBandY + 1,
                      renderer.truncatedText(SMALL_FONT_ID, statusLine, w - pad * 2 - 10).c_str(), true,
                      EpdFontFamily::BOLD);
  }

  const int contentTop = statusBandY + statusBandH + 4;
  const int hintTop = pageH - renderer.getLineHeight(SMALL_FONT_ID) - 16;
  const bool portrait = pageH > w;

  // The row list is also the visible carousel: the current book and carried
  // study items use the same large content area instead of competing cards.
  const int selectedIndex = overviewCursor >= 0 && overviewCursor < n ? overviewCursor : 0;
  const int selectedPocketIndex = n > 0 && rows[selectedIndex].pocket ? selectedIndex : -1;
  char carouselPosition[16] = {0};
  if (n > 1) snprintf(carouselPosition, sizeof(carouselPosition), "%d/%d", selectedIndex + 1, n);

  auto sectionHeader = [&](int x, int y, int cw, const char* label, const char* right) -> int {
    const int labelFont = fontForText(SMALL_FONT_ID, label);
    renderer.drawText(labelFont, x, y, label, true, EpdFontFamily::BOLD);
    int rightW = 0;
    int headerH = renderer.getLineHeight(labelFont);
    if (right && right[0]) {
      const int rightFont = fontForText(SMALL_FONT_ID, right);
      rightW = renderer.getTextWidth(rightFont, right);
      renderer.drawText(rightFont, x + cw - rightW, y, right, true);
      headerH = std::max(headerH, renderer.getLineHeight(rightFont));
    }
    const int labelW = renderer.getTextWidth(labelFont, label, EpdFontFamily::BOLD);
    const int ruleStart = x + labelW + 9;
    const int ruleEnd = x + cw - (rightW ? rightW + 9 : 0);
    if (ruleStart < ruleEnd) renderer.drawLine(ruleStart, y + headerH / 2, ruleEnd, y + headerH / 2);
    return y + headerH + 7;
  };

  auto drawReading = [&](int x, int y, int cw, int ch) {
    const int panelBottom = y + ch;
    renderer.drawRect(x, y, cw, ch, 2, true);
    const bool hasBook = renderReadingSnapshot.valid;
    if (!hasBook) {
      y = sectionHeader(x + 12, y + 10, cw - 24, tr(STR_POCKET_CONTINUE_READING), carouselPosition);
      renderer.drawCenteredText(UI_10_FONT_ID, y + (panelBottom - y) / 2 - line10, tr(STR_NO_OPEN_BOOK), true,
                                EpdFontFamily::BOLD);
      renderer.drawCenteredText(SMALL_FONT_ID, y + (panelBottom - y) / 2 + 6, tr(STR_START_READING), true);
      return;
    }

    // One flat editorial grid: artwork and metadata share the same top/bottom
    // baselines. Removing the floating slab makes the enlarged cover feel like
    // part of the page instead of a card placed on top of another card.
    const int coverW = std::min(cw * 60 / 100, std::max(cw * 47 / 100, ch * 2 / 3));
    drawReadingCover(x, y, coverW, ch);
    const int metaX = x + coverW;
    renderer.drawLine(metaX, y, metaX, panelBottom, 2, true);
    const int textX = metaX + 14;
    const int textW = x + cw - textX - 13;
    const char* readingLabel = tr(STR_POCKET_CONTINUE_READING);
    const int labelFont = fontForText(SMALL_FONT_ID, readingLabel);
    renderer.drawText(labelFont, textX, y + 14,
                      renderer.truncatedText(labelFont, readingLabel, textW - (carouselPosition[0] ? 34 : 0)).c_str(),
                      true, EpdFontFamily::BOLD);
    if (carouselPosition[0]) {
      const int posW = renderer.getTextWidth(SMALL_FONT_ID, carouselPosition);
      renderer.drawText(SMALL_FONT_ID, x + cw - 13 - posW, y + 14, carouselPosition, true);
    }
    const int titleFont = fontForText(UI_12_FONT_ID, renderReadingSnapshot.title);
    auto titleLines = renderer.wrappedText(titleFont, renderReadingSnapshot.title, textW, 4, EpdFontFamily::BOLD);
    int ty = y + 14 + renderer.getLineHeight(labelFont) + 22;
    for (const auto& titleLine : titleLines) {
      renderer.drawText(titleFont, textX, ty, titleLine.c_str(), true, EpdFontFamily::BOLD);
      ty += renderer.getLineHeight(titleFont) + 2;
    }
    if (renderReadingSnapshot.author[0]) {
      const int authorFont = fontForText(SMALL_FONT_ID, renderReadingSnapshot.author);
      ty += 5;
      renderer.drawText(authorFont, textX, ty,
                        renderer.truncatedText(authorFont, renderReadingSnapshot.author, textW).c_str(), true);
    }
    if (renderReadingSnapshot.percent >= 0) {
      char pct[8];
      snprintf(pct, sizeof(pct), "%d%%", renderReadingSnapshot.percent);
      const int progressY = panelBottom - line12 - 24;
      renderer.drawText(UI_12_FONT_ID, textX, progressY, pct, true, EpdFontFamily::BOLD);
      const char* resume = "RESUME";
      const int resumeW = renderer.getTextWidth(SMALL_FONT_ID, resume, EpdFontFamily::BOLD);
      renderer.drawText(SMALL_FONT_ID, textX + textW - resumeW, progressY + line12 - line10, resume, true,
                        EpdFontFamily::BOLD);
      const int barY = panelBottom - 10;
      renderer.drawLine(textX, barY, textX + textW, barY);
      const int fillW = textW * renderReadingSnapshot.percent / 100;
      if (fillW > 0) renderer.drawLine(textX, barY - 2, textX + fillW, barY - 2, 4, true);
    }
  };

  auto drawStudy = [&](int x, int y, int cw, int ch) {
    if (cw <= 16 || ch <= 16) return;
    const int panelBottom = y + ch;
    renderer.drawRect(x, y, cw, ch, 2, true);
    if (selectedPocketIndex < 0) {
      y = sectionHeader(x + 12, y + 10, cw - 24, tr(STR_POCKET_STUDY), nullptr);
      const int emptyFont = fontForText(UI_10_FONT_ID, tr(STR_POCKET_EMPTY));
      const int emptyAdvance = renderer.getLineHeight(emptyFont) + 3;
      const int emptyY = y + std::max(0, (panelBottom - y - emptyAdvance * 2) / 2);
      drawWrappedFixed(renderer, emptyFont, x + 12, emptyY, tr(STR_POCKET_EMPTY), cw - 24, 2, emptyAdvance);
      return;
    }
    const OverviewRow& row = rows[selectedPocketIndex];
    y = sectionHeader(x + 12, y + 10, cw - 24, tr(STR_POCKET_STUDY), carouselPosition);
    const int titleFont = fontForText(UI_12_FONT_ID, row.project);
    renderer.drawText(titleFont, x + 12, y,
                      renderer.truncatedText(titleFont, row.project, cw - 24, EpdFontFamily::BOLD).c_str(), true,
                      EpdFontFamily::BOLD);
    y += renderer.getLineHeight(titleFont) + 8;
    if (y + 8 >= panelBottom) return;
    const int bodyFont = fontForText(UI_10_FONT_ID, row.activity);
    const int advance = renderer.getLineHeight(bodyFont) + 3;
    int maxLines = (panelBottom - y - 8) / advance;
    if (maxLines < 1) return;
    if (maxLines > (portrait ? 7 : 8)) maxLines = portrait ? 7 : 8;
    drawWrappedFixed(renderer, bodyFont, x + 12, y, row.activity, cw - 24, maxLines, advance);
  };

  auto drawUtilities = [&](int x, int y, int cw, int ch) {
    const int panelBottom = y + ch;
    renderer.drawRect(x, y, cw, ch, 2, true);
    const int inset = 12;
    x += inset;
    y += 10;
    cw -= inset * 2;
    const bool hasEvent = renderGlanceSnapshot.eventCount > 0;
    const int eventH = hasEvent ? line10 + line12 + 18 : 0;
    const int weatherBottom = panelBottom - 9 - eventH;
    char line[96];
    if (renderGlanceSnapshot.weather.valid) {
      const char* place =
          renderGlanceSnapshot.weather.place[0] ? renderGlanceSnapshot.weather.place : tr(STR_POCKET_WEATHER);
      const int placeFont = fontForText(SMALL_FONT_ID, place);
      renderer.drawText(placeFont, x, y, renderer.truncatedText(placeFont, place, cw * 2 / 3).c_str(), true,
                        EpdFontFamily::BOLD);
      char weatherStamp[20] = {0};
      if (formatWeatherSnapshotDate(weatherStamp, sizeof(weatherStamp), renderGlanceSnapshot.weather)) {
        if (snapshotIsStale(renderSavedEpoch))
          strncat(weatherStamp, " SAVED", sizeof(weatherStamp) - strlen(weatherStamp) - 1);
        const int stampW = renderer.getTextWidth(SMALL_FONT_ID, weatherStamp, EpdFontFamily::BOLD);
        renderer.drawText(SMALL_FONT_ID, x + cw - stampW, y, weatherStamp, true, EpdFontFamily::BOLD);
      }

      const int nowY = y + renderer.getLineHeight(placeFont) + 5;
      const int forecastRoom = weatherBottom - nowY - 76;
      const int forecastH =
          renderGlanceSnapshot.weather.dayCount >= 2 && forecastRoom >= 106 ? std::min(148, forecastRoom) : 0;
      const int forecastY = weatherBottom - forecastH;
      const int nowW = cw * 62 / 100;
      drawCurrentWeatherVisual(renderer, renderGlanceSnapshot.weather, x, nowY, nowW,
                               std::max(46, forecastY - nowY - 4));

      int rainChars = AgentDeck::GlanceFormat::formatRainLine(line, sizeof(line), renderGlanceSnapshot.weather);
      if (rainChars == 0) rainChars = formatNextRainDay(line, sizeof(line), renderGlanceSnapshot.weather);
      if (rainChars > 0) {
        const int rainX = x + cw * 68 / 100;
        const int rainW = cw - (rainX - x);
        renderer.drawLine(rainX - 8, y + 2, rainX - 8, std::max(y + 10, std::min(forecastY - 3, nowY + 66)), 3, true);
        const int rainFont = fontForText(UI_10_FONT_ID, line);
        drawWrappedFixed(renderer, rainFont, rainX, y + 1, line, rainW, 2, line10 + 2, EpdFontFamily::BOLD);
      }
      if (forecastH > 0) drawForecastRibbon(renderer, renderGlanceSnapshot.weather, x, forecastY, cw, forecastH);
    } else {
      renderer.drawText(SMALL_FONT_ID, x, y, tr(STR_POCKET_NO_WEATHER), true);
    }

    if (hasEvent) {
      const int eventTop = panelBottom - eventH;
      renderer.drawLine(x, eventTop, x + cw, eventTop);
      int ey = sectionHeader(x, eventTop + 5, cw, tr(STR_POCKET_NEXT_EVENT), nullptr);
      if (AgentDeck::GlanceFormat::formatEventLine(line, sizeof(line), renderGlanceSnapshot.events[0]) > 0) {
        const int eventFont = fontForText(UI_10_FONT_ID, line);
        renderer.drawText(eventFont, x, ey, renderer.truncatedText(eventFont, line, cw, EpdFontFamily::BOLD).c_str(),
                          true, EpdFontFamily::BOLD);
      }
    }
  };

  const int primaryGap = 8;
  const int availableH = hintTop - contentTop;
  int primaryX = pad;
  int primaryY = contentTop;
  int primaryW = w - pad * 2;
  int primaryH = availableH;
  if (portrait) {
    // Recovered chrome space belongs to the hero. This still fits the full
    // five-day graph, then returns the remainder to cover and progress.
    const int utilityH = std::max(250, std::min(272, availableH * 40 / 100));
    primaryH = availableH - utilityH - primaryGap;
    if (n > 0 && rows[selectedIndex].reading)
      drawReading(primaryX, primaryY, primaryW, primaryH);
    else
      drawStudy(primaryX, primaryY, primaryW, primaryH);
    drawUtilities(pad, primaryY + primaryH + primaryGap, w - pad * 2, utilityH);
  } else {
    const int colGap = 10;
    primaryW = (w - pad * 2 - colGap) * 54 / 100;
    if (n > 0 && rows[selectedIndex].reading)
      drawReading(primaryX, primaryY, primaryW, primaryH);
    else
      drawStudy(primaryX, primaryY, primaryW, primaryH);
    drawUtilities(primaryX + primaryW + colGap, primaryY, w - pad - (primaryX + primaryW + colGap), primaryH);
  }

  if (sidePaging) drawPocketSideChevrons(renderer);

  // Confirm selects the current carousel item; Read and Study no longer need
  // dedicated keys. Right remains explicit manual sync for offline recovery.
  drawPocketActionStrip(renderer, tr(STR_POCKET_LIBRARY), tr(STR_SELECT), tr(STR_POCKET_SYNC));
  renderer.displayBuffer();
}

void PocketDailyActivity::renderDetail() {
  const auto& m = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();
  const int pad = m.contentSidePadding;
  const int line10 = renderer.getLineHeight(UI_10_FONT_ID);
  const int lineS = renderer.getLineHeight(SMALL_FONT_ID);

  // Snapshot the selected session + its timeline entries under one lock.
  char project[40] = {0}, agentType[16] = {0}, model[32] = {0}, state[20] = {0}, tool[40] = {0};
  char activity[AgentDeck::SESSION_ACTIVITY_CAP] = {0};
  uint32_t elapsed = 0;
  bool found = false;
  char tlText[AgentDeck::DashboardState::TIMELINE_CAP][96];
  char tlType[AgentDeck::DashboardState::TIMELINE_CAP][20];
  uint32_t tlTs[AgentDeck::DashboardState::TIMELINE_CAP];
  uint32_t epochSec = 0, epochAtMs = 0;
  int tlCount = 0;
  AgentDeck::lockState();
  const auto& s = AgentDeck::g_state;
  for (uint8_t i = 0; i < s.sessionCount; i++) {
    if (selectedSid[0] && strcmp(s.sessions[i].id, selectedSid) == 0) {
      const auto& se = s.sessions[i];
      strncpy(project, se.projectName, sizeof(project) - 1);
      strncpy(agentType, se.agentType, sizeof(agentType) - 1);
      strncpy(model, se.modelName, sizeof(model) - 1);
      strncpy(state, se.state, sizeof(state) - 1);
      strncpy(tool, se.currentTool, sizeof(tool) - 1);
      strncpy(activity, se.activity, sizeof(activity) - 1);
      elapsed = se.elapsedSec;
      found = true;
      break;
    }
  }
  if (!found) {  // observed/single-session fallback → render the focused state
    strncpy(project, s.projectName, sizeof(project) - 1);
    strncpy(agentType, s.agentType, sizeof(agentType) - 1);
    strncpy(model, s.modelName, sizeof(model) - 1);
    strncpy(tool, s.currentTool, sizeof(tool) - 1);
    strncpy(activity, s.currentTool, sizeof(activity) - 1);
    strncpy(state, agentStateLabel(s.state), sizeof(state) - 1);
    found = s.dataReceived;
  }
  // Matching timeline entries, oldest → newest (ring: head is oldest when full).
  // Unattributed rows (empty sid) are global error/scheduled signals — show them
  // in every session's Detail, matching the other dashboard surfaces.
  const int cnt = s.timelineCount;
  for (int k = 0; k < cnt; k++) {
    int idx = (s.timelineCount < AgentDeck::DashboardState::TIMELINE_CAP)
                  ? k
                  : (s.timelineHead + k) % AgentDeck::DashboardState::TIMELINE_CAP;
    const AgentDeck::TimelineItem& t = s.timeline[idx];
    if (t.sid[0] != '\0' && selectedSid[0] && strcmp(rawSid(t.sid), rawSid(selectedSid)) != 0) continue;
    if (t.text[0] == '\0') continue;
    strncpy(tlText[tlCount], t.text, sizeof(tlText[0]) - 1);
    tlText[tlCount][sizeof(tlText[0]) - 1] = '\0';
    strncpy(tlType[tlCount], t.type, sizeof(tlType[0]) - 1);
    tlType[tlCount][sizeof(tlType[0]) - 1] = '\0';
    tlTs[tlCount] = t.tsSec;
    if (++tlCount >= AgentDeck::DashboardState::TIMELINE_CAP) break;
  }
  epochSec = s.daemonEpochSec;
  epochAtMs = s.daemonEpochAtMs;
  AgentDeck::unlockState();

  // Turn grouping (mirrors the Apple/Android projection): a chat_start whose
  // turn has already completed is redundant with its chat_end/chat_response
  // row — showing both renders every turn twice. Keep only in-flight starts.
  bool tlShow[AgentDeck::DashboardState::TIMELINE_CAP];
  for (int k = 0; k < tlCount; k++) {
    tlShow[k] = true;
    if (strcmp(tlType[k], "chat_start") != 0) continue;
    for (int j = k + 1; j < tlCount; j++) {
      if (strcmp(tlType[j], "chat_end") == 0 || strcmp(tlType[j], "chat_response") == 0) {
        tlShow[k] = false;
        break;
      }
    }
  }

  // Estimated "daemon now" for per-entry ages (no RTC on this device).
  const uint32_t daemonNowSec = epochSec ? epochSec + (millis() - epochAtMs) / 1000UL : 0;

  renderer.clearScreen();
  drawBrandedHeader("Session", nullptr);
  int y = m.topPadding + m.headerHeight + m.verticalSpacing;

  if (!found) {
    renderer.drawText(UI_10_FONT_ID, pad, y, tr(STR_POCKET_SESSION_ENDED), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels("Back", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // Title row: glyph + project.
  const uint8_t* g = glyphForAgent(agentType);
  int textX = pad;
  if (g) {
    renderer.drawIcon(g, pad, y, kGlyphPx, kGlyphPx);
    textX = pad + kGlyphPx + 12;
  }
  renderer.drawText(
      UI_10_FONT_ID, textX, y + (kGlyphPx - line10) / 2,
      renderer.truncatedText(UI_10_FONT_ID, project[0] ? project : "session", w - textX - pad, EpdFontFamily::BOLD)
          .c_str(),
      true, EpdFontFamily::BOLD);
  y += (g ? kGlyphPx : line10) + 8;

  // Compact one-line meta: agent · model · state (· elapsed).
  char meta[140];
  int mo = snprintf(meta, sizeof(meta), "%s", agentType[0] ? agentType : "agent");
  if (model[0]) mo += snprintf(meta + mo, sizeof(meta) - mo, " \xC2\xB7 %s", model);
  if (state[0]) mo += snprintf(meta + mo, sizeof(meta) - mo, " \xC2\xB7 %s", wireStateLabel(state));
  if (elapsed > 0) {
    if (elapsed >= 60)
      mo += snprintf(meta + mo, sizeof(meta) - mo, " \xC2\xB7 %um", (unsigned)(elapsed / 60));
    else
      mo += snprintf(meta + mo, sizeof(meta) - mo, " \xC2\xB7 %us", (unsigned)elapsed);
  }
  renderer.drawText(SMALL_FONT_ID, pad, y, renderer.truncatedText(SMALL_FONT_ID, meta, w - pad * 2).c_str(), true);
  y += lineS + 6;
  if (activity[0]) {
    renderer.drawText(SMALL_FONT_ID, pad, y, tr(STR_POCKET_CURRENT_WORK), true, EpdFontFamily::BOLD);
    y += lineS + 2;
    const int activityFont = fontForText(SMALL_FONT_ID, activity);
    const int activityAdvance = renderer.getLineHeight(activityFont) + 2;
    const int activityLines =
        drawWrappedFixed(renderer, activityFont, pad, y, activity, w - pad * 2, 3, activityAdvance);
    y += activityLines * activityAdvance + 4;
  } else if (tool[0]) {
    char tl[80];
    snprintf(tl, sizeof(tl), "Now: %s", tool);
    renderer.drawText(SMALL_FONT_ID, pad, y, renderer.truncatedText(SMALL_FONT_ID, tl, w - pad * 2).c_str(), true);
    y += lineS + 6;
  }

  // Collect the attention state for this session. A real requestId gate gets
  // Allow/Deny; a managed prompt gets only its correlated daemon options; an
  // observed terminal prompt stays read-only and says so explicitly.
  AwaitingItem attention = {};
  const bool awaiting = findAwaiting(selectedSid, attention);
  const AgentDeck::AttentionMode attentionMode = awaiting ? attention.attentionMode : AgentDeck::AttentionMode::None;
  char optLabels[8][80];
  int optCount = 0;
  if (attentionMode == AgentDeck::AttentionMode::RealOptions) {
    AgentDeck::lockState();
    const auto& ds = AgentDeck::g_state;
    if (selectedSid[0] && ds.optionSessionId[0] && strcmp(selectedSid, ds.optionSessionId) == 0) {
      optCount = ds.optionCount;
      if (optCount > 8) optCount = 8;
      for (int i = 0; i < optCount; i++) {
        strncpy(optLabels[i], ds.options[i].label, sizeof(optLabels[0]) - 1);
        optLabels[i][sizeof(optLabels[0]) - 1] = '\0';
      }
    }
    AgentDeck::unlockState();
  } else if (attentionMode == AgentDeck::AttentionMode::PermissionGate) {
    optCount = 2;
    strncpy(optLabels[0], "Allow", sizeof(optLabels[0]) - 1);
    strncpy(optLabels[1], "Deny", sizeof(optLabels[0]) - 1);
    optLabels[0][sizeof(optLabels[0]) - 1] = '\0';
    optLabels[1][sizeof(optLabels[0]) - 1] = '\0';
  }

  // ── Scrollable content: timeline (oldest→newest) then the decision block ──
  renderer.drawLine(pad, y, w - pad, y);
  y += 8;
  const char* activityHeading = "Activity";
  if (AgentDeck::attentionIsActionable(attentionMode))
    activityHeading = "Activity \xC2\xB7 scroll down to decide";
  else if (attentionMode == AgentDeck::AttentionMode::RespondInTerminal)
    activityHeading = "Activity \xC2\xB7 terminal response required";
  else if (attentionMode == AgentDeck::AttentionMode::WaitingForOptions)
    activityHeading = "Activity \xC2\xB7 loading choices";
  renderer.drawText(SMALL_FONT_ID, pad, y, activityHeading, true, EpdFontFamily::BOLD);
  y += lineS + 4;

  const int listTop = y;
  const int listBottom = pageH - m.buttonHintsHeight - 8;

  // Flat line list. lineOpt: -1 normal, -2 heading (bold), >=0 = option index.
  // Reserve the worst case up front (timeline entries × 3 wrap lines + decision
  // block) — this repaints on every state change, and unreserved push_back growth
  // fragments DRAM (CLAUDE.md rule 7).
  const size_t maxLines = static_cast<size_t>(tlCount) * 3 + 12 + static_cast<size_t>(optCount);
  std::vector<std::string> lines;
  std::vector<int> lineFonts;
  std::vector<int> lineOpt;
  lines.reserve(maxLines);
  lineFonts.reserve(maxLines);
  lineOpt.reserve(maxLines);
  int tlShown = 0;
  for (int k = 0; k < tlCount; k++) {  // chronological
    if (!tlShow[k]) continue;
    // Row prefix: "[OK] 5m " — type marker (shared EINK_ICON_GLYPHS vocabulary)
    // plus the entry age from the daemon-clock estimate. Continuation lines
    // indent under the text.
    char pfx[16];
    if (daemonNowSec && tlTs[k] && daemonNowSec >= tlTs[k]) {
      char age[8];
      formatAge(daemonNowSec - tlTs[k], age, sizeof(age));
      snprintf(pfx, sizeof(pfx), "%s %s ", timelineGlyph(tlType[k]), age);
    } else {
      snprintf(pfx, sizeof(pfx), "%s ", timelineGlyph(tlType[k]));
    }
    const int fid = fontForText(SMALL_FONT_ID, tlText[k]);
    const int pfxW = renderer.getTextWidth(SMALL_FONT_ID, pfx);
    auto wrapped = renderer.wrappedText(fid, tlText[k], w - pad * 2 - pfxW, 3);
    for (size_t li = 0; li < wrapped.size(); li++) {
      lines.push_back((li == 0 ? std::string(pfx) : std::string("   ")) + wrapped[li]);
      lineFonts.push_back(fid);
      lineOpt.push_back(-1);
    }
    tlShown++;
  }
  if (tlShown == 0) {
    lines.push_back("Detailed events will appear as work progresses.");
    lineFonts.push_back(SMALL_FONT_ID);
    lineOpt.push_back(-1);
  }
  if (awaiting) {
    lines.push_back("");
    lineFonts.push_back(SMALL_FONT_ID);
    lineOpt.push_back(-1);
    lines.push_back(AgentDeck::attentionIsActionable(attentionMode) ? "Needs your decision:" : "Needs your attention:");
    lineFonts.push_back(SMALL_FONT_ID);
    lineOpt.push_back(-2);
    if (attention.question[0]) {
      const int qf = fontForText(SMALL_FONT_ID, attention.question);
      auto qlines = renderer.wrappedText(qf, attention.question, w - pad * 2, 4);
      for (auto& ql : qlines) {
        lines.push_back(ql);
        lineFonts.push_back(qf);
        lineOpt.push_back(-1);
      }
    }
    if (attentionMode == AgentDeck::AttentionMode::RespondInTerminal) {
      lines.push_back("Respond in the agent terminal.");
      lineFonts.push_back(SMALL_FONT_ID);
      lineOpt.push_back(-2);
      lines.push_back("This observed session did not expose its choices remotely.");
      lineFonts.push_back(SMALL_FONT_ID);
      lineOpt.push_back(-1);
    } else if (attentionMode == AgentDeck::AttentionMode::WaitingForOptions) {
      lines.push_back("Loading choices from the managed session...");
      lineFonts.push_back(SMALL_FONT_ID);
      lineOpt.push_back(-1);
    }
    for (int i = 0; i < optCount; i++) {
      lines.push_back(optLabels[i]);
      lineFonts.push_back(fontForText(SMALL_FONT_ID, optLabels[i]));
      lineOpt.push_back(i);
    }
  }

  const int visibleLines = (listBottom - listTop) / lineS;
  detailMaxScroll = (int)lines.size() - visibleLines;
  if (detailMaxScroll < 0) detailMaxScroll = 0;
  if (detailScroll > detailMaxScroll) detailScroll = detailMaxScroll;
  if (detailScroll < 0) detailScroll = 0;

  int ly = listTop;
  for (int i = detailScroll; i < (int)lines.size(); i++) {
    const int lh = renderer.getLineHeight(lineFonts[i]);
    if (ly + lh > listBottom) break;
    const bool isOpt = lineOpt[i] >= 0;
    const bool sel = isOpt && lineOpt[i] == optionCursor;
    const auto style = (lineOpt[i] == -2 || sel) ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    if (sel) {  // highlighted option: inverted bar + caret
      renderer.fillRect(pad - 4, ly - 1, w - pad * 2 + 8, lh, true);
      char row[100];
      snprintf(row, sizeof(row), "\xE2\x96\xB6 %s", lines[i].c_str());
      renderer.drawText(lineFonts[i], pad, ly, row, false, style);
    } else if (isOpt) {
      char row[100];
      snprintf(row, sizeof(row), "  %s", lines[i].c_str());
      renderer.drawText(lineFonts[i], pad, ly, row, true, style);
    } else {
      renderer.drawText(lineFonts[i], pad, ly, lines[i].c_str(), true, style);
    }
    ly += lh;
  }

  // Hint bar. OK selects the highlighted option only once scrolled to the
  // decision (atBottom); the heading nudges the user to scroll down to it.
  const bool atBottom = detailScroll >= detailMaxScroll;
  const auto labels = mappedInput.mapLabels(
      "Back", (AgentDeck::attentionIsActionable(attentionMode) && optCount > 0 && atBottom) ? "Select" : "", "Up",
      "Down");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void PocketDailyActivity::renderPocketCard(const PocketDaily::Card& card) {
  const auto& m = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();
  const int pad = m.contentSidePadding;
  const int lineS = renderer.getLineHeight(SMALL_FONT_ID);

  renderer.clearScreen();
  // The item title is content; the shell remains visibly Pocket even though a
  // local daemon authored the payload.
  drawBrandedHeader(card.title[0] ? card.title : tr(STR_POCKET_TITLE), tr(STR_POCKET_SUBTITLE));
  int y = m.topPadding + m.headerHeight + m.verticalSpacing + 8;

  const int qFont = fontForText(UI_12_FONT_ID, card.question);
  const int qAdvance = renderer.getLineHeight(qFont) + 5;
  const int optionAdvance = renderer.getLineHeight(UI_10_FONT_ID) + 7;
  const int hintTop = pageH - m.buttonHintsHeight - 6;
  const int optionsTop = hintTop - card.choiceCount * optionAdvance - 6;
  auto qLines = renderer.wrappedText(qFont, card.question, w - pad * 2, 6);
  for (const auto& line : qLines) {
    if (y + qAdvance > optionsTop - lineS - 10) break;
    renderer.drawText(qFont, pad, y, line.c_str(), true, EpdFontFamily::BOLD);
    y += qAdvance;
  }
  if (card.context[0] && y + lineS < optionsTop - 4) {
    y += 5;
    const int contextFont = fontForText(SMALL_FONT_ID, card.context);
    auto lines = renderer.wrappedText(contextFont, card.context, w - pad * 2, 4);
    for (const auto& line : lines) {
      if (y + renderer.getLineHeight(contextFont) > optionsTop - 4) break;
      renderer.drawText(contextFont, pad, y, line.c_str(), true);
      y += renderer.getLineHeight(contextFont) + 2;
    }
  }

  y = optionsTop;
  char hints[3][24] = {{0}, {0}, {0}};
  for (uint8_t i = 0; i < card.choiceCount; i++) {
    const char* label = card.choices[i].label;
    const int font = fontForText(UI_10_FONT_ID, label);
    char row[64];
    snprintf(row, sizeof(row), "[%u] %s", (unsigned)(i + 2), label);
    renderer.drawText(font, pad, y, renderer.truncatedText(font, row, w - pad * 2).c_str(), true);
    if (hasCJK(label) || strlen(label) >= sizeof(hints[i]))
      snprintf(hints[i], sizeof(hints[i]), "%u", (unsigned)(i + 2));
    else
      snprintf(hints[i], sizeof(hints[i]), "%s", renderer.truncatedText(UI_10_FONT_ID, label, 96).c_str());
    y += renderer.getLineHeight(font) + 7;
  }
  GUI.drawButtonHints(renderer, card.choiceCount == 0 ? tr(STR_POCKET_DONE) : tr(STR_POCKET_LATER), hints[0], hints[1],
                      hints[2]);
  renderer.displayBuffer();
}

void PocketDailyActivity::renderCard() {
  PocketDaily::Card pocket{};
  if (findPocketCard(cardSid, pocket)) {
    renderPocketCard(pocket);
    return;
  }
  const auto& m = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();
  const int pad = m.contentSidePadding;
  const int lineS = renderer.getLineHeight(SMALL_FONT_ID);

  AwaitingItem item = {};
  const bool present = findAwaiting(cardSid, item) && item.attentionMode != AgentDeck::AttentionMode::None;
  const AgentDeck::AttentionMode mode = present ? item.attentionMode : AgentDeck::AttentionMode::None;

  // Session meta + correlated option labels under one lock.
  char project[40] = {0}, agentType[16] = {0};
  char activityLine[AgentDeck::SESSION_ACTIVITY_CAP] = {0};
  char optLabels[8][80];
  bool optRecommended[8] = {false};
  int optCount = 0;
  AgentDeck::lockState();
  {
    const auto& s = AgentDeck::g_state;
    for (uint8_t i = 0; i < s.sessionCount; i++) {
      if (cardSid[0] && strcmp(s.sessions[i].id, cardSid) == 0) {
        strncpy(project, s.sessions[i].projectName, sizeof(project) - 1);
        strncpy(agentType, s.sessions[i].agentType, sizeof(agentType) - 1);
        strncpy(activityLine, s.sessions[i].activity, sizeof(activityLine) - 1);
        break;
      }
    }
    if (!project[0]) strncpy(project, s.projectName, sizeof(project) - 1);
    if (!agentType[0]) strncpy(agentType, s.agentType, sizeof(agentType) - 1);
    if (mode == AgentDeck::AttentionMode::RealOptions && s.optionSessionId[0] &&
        strcmp(s.optionSessionId, cardSid) == 0) {
      optCount = s.optionCount > 8 ? 8 : s.optionCount;
      for (int i = 0; i < optCount; i++) {
        strncpy(optLabels[i], s.options[i].label, sizeof(optLabels[0]) - 1);
        optLabels[i][sizeof(optLabels[0]) - 1] = '\0';
        optRecommended[i] = s.options[i].recommended;
      }
    }
  }
  AgentDeck::unlockState();
  if (mode == AgentDeck::AttentionMode::PermissionGate) {
    // applyDecision cursor order: 0 = Allow, 1 = Deny.
    optCount = 2;
    snprintf(optLabels[0], sizeof(optLabels[0]), "Allow");
    snprintf(optLabels[1], sizeof(optLabels[1]), "Deny");
    optRecommended[0] = optRecommended[1] = false;
  }

  renderer.clearScreen();
  drawBrandedHeader("Decision", nullptr);
  int y = m.topPadding + m.headerHeight + m.verticalSpacing;

  if (!present) {  // resolved between service ticks; serviceCard flips home next tick
    renderer.drawText(UI_10_FONT_ID, pad, y, tr(STR_POCKET_RESOLVED), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  // Title row: glyph + project.
  const int line10 = renderer.getLineHeight(UI_10_FONT_ID);
  const uint8_t* g = glyphForAgent(agentType);
  int textX = pad;
  if (g) {
    renderer.drawIcon(g, pad, y, kGlyphPx, kGlyphPx);
    textX = pad + kGlyphPx + 12;
  }
  renderer.drawText(
      UI_10_FONT_ID, textX, y + (kGlyphPx - line10) / 2,
      renderer.truncatedText(UI_10_FONT_ID, project[0] ? project : "session", w - textX - pad, EpdFontFamily::BOLD)
          .c_str(),
      true, EpdFontFamily::BOLD);
  y += (g ? kGlyphPx : line10) + 6;

  const char* metaTail = AgentDeck::attentionIsActionable(mode) ? "needs your decision" : "needs your attention";
  char meta[80];
  snprintf(meta, sizeof(meta), "%s \xC2\xB7 %s", agentType[0] ? agentType : "agent", metaTail);
  renderer.drawText(SMALL_FONT_ID, pad, y, renderer.truncatedText(SMALL_FONT_ID, meta, w - pad * 2).c_str(), true);
  y += lineS + 6;
  renderer.drawLine(pad, y, w - pad, y);
  y += 10;

  const bool softkeys = cardUsesSoftkeys(mode, item.optionCount);

  // ── Options block: measured first so the question knows its floor ──
  // Softkey grammar rows are "[n] label" where n is the physical button slot
  // (2..4; slot 1 is Later). Cursor grammar rows are "▶ label" with highlight.
  const int optFontBase = UI_10_FONT_ID;
  int rowFonts[8];
  int rowsH = 0;
  for (int i = 0; i < optCount; i++) {
    rowFonts[i] = fontForText(optFontBase, optLabels[i]);
    rowsH += renderer.getLineHeight(rowFonts[i]) + 6;
  }
  int noteLines = 0;  // read-only card note (WaitingForOptions / RespondInTerminal)
  const char* note = nullptr;
  if (mode == AgentDeck::AttentionMode::WaitingForOptions) {
    note = "Loading choices from the managed session...";
    noteLines = 1;
  } else if (mode == AgentDeck::AttentionMode::RespondInTerminal) {
    note = "Respond in the agent terminal \xE2\x80\x94 this observed session did not expose its choices remotely.";
    noteLines = 2;
  }
  const int hintTop = pageH - m.buttonHintsHeight - 6;
  const int optionsTop = hintTop - rowsH - noteLines * (lineS + 2) - 4;

  // ── Question — the card's center. Bigger face, CJK-aware, floor-clamped ──
  const char* q = item.question[0]
                      ? item.question
                      : (mode == AgentDeck::AttentionMode::PermissionGate ? "Approve this tool call?" : "Your turn.");
  const int qFont = fontForText(UI_12_FONT_ID, q);
  const int qAdvance = renderer.getLineHeight(qFont) + 4;
  {
    auto qLines = renderer.wrappedText(qFont, q, w - pad * 2, 6);
    for (auto& ql : qLines) {
      if (y + qAdvance > optionsTop - lineS - 8) break;  // keep room for context
      renderer.drawText(qFont, pad, y, ql.c_str(), true, EpdFontFamily::BOLD);
      y += qAdvance;
    }
  }
  y += 4;
  // Context: what the agent was doing (one line, quiet).
  if (activityLine[0] && y + lineS <= optionsTop - 4) {
    const int aFont = fontForText(SMALL_FONT_ID, activityLine);
    renderer.drawText(aFont, pad, y, renderer.truncatedText(aFont, activityLine, w - pad * 2).c_str(), true);
  }

  // ── Draw the options block bottom-anchored ──
  y = optionsTop;
  if (note) {
    auto nLines = renderer.wrappedText(SMALL_FONT_ID, note, w - pad * 2, noteLines);
    for (auto& nl : nLines) {
      renderer.drawText(SMALL_FONT_ID, pad, y, nl.c_str(), true);
      y += lineS + 2;
    }
  }
  for (int i = 0; i < optCount; i++) {
    const int lh = renderer.getLineHeight(rowFonts[i]);
    char row[96];
    if (softkeys) {
      // Physical slot numbers: PermissionGate puts Deny on 3, Allow on 4;
      // options map in order onto 2..4. Keep in lockstep with handleButtons.
      const int keycap = (mode == AgentDeck::AttentionMode::PermissionGate) ? (i == 0 ? 4 : 3) : (2 + i);
      snprintf(row, sizeof(row), "[%d] %s", keycap, optLabels[i]);
      renderer.drawText(rowFonts[i], pad, y, renderer.truncatedText(rowFonts[i], row, w - pad * 2).c_str(), true,
                        optRecommended[i] ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    } else {
      const bool sel = (i == optionCursor);
      if (sel) {
        renderer.fillRect(pad - 4, y - 1, w - pad * 2 + 8, lh + 2, true);
        snprintf(row, sizeof(row), "\xE2\x96\xB6 %s", optLabels[i]);
        renderer.drawText(rowFonts[i], pad, y, renderer.truncatedText(rowFonts[i], row, w - pad * 2).c_str(), false,
                          EpdFontFamily::BOLD);
      } else {
        snprintf(row, sizeof(row), "  %s", optLabels[i]);
        renderer.drawText(rowFonts[i], pad, y, renderer.truncatedText(rowFonts[i], row, w - pad * 2).c_str(), true,
                          optRecommended[i] ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
      }
    }
    y += lh + 6;
  }

  // ── Hint bar ──
  if (softkeys) {
    // Positional labels matching getPressedFrontButton's raw physical order —
    // deliberately NOT mapLabels(), which applies the user's logical remap.
    char h2[24] = {0}, h3[24] = {0}, h4[24] = {0};
    const auto hintFor = [&](char* out, size_t cap, const char* label, int keycap) {
      if (!label || !label[0]) {
        out[0] = '\0';
        return;
      }
      if (!hasCJK(label)) {
        snprintf(out, cap, "%s", renderer.truncatedText(UI_10_FONT_ID, label, 96).c_str());
      } else {
        snprintf(out, cap, "%d", keycap);  // body row carries the CJK label
      }
    };
    if (mode == AgentDeck::AttentionMode::PermissionGate) {
      GUI.drawButtonHints(renderer, "Later", "Detail", "Deny", "Allow");
    } else if (mode == AgentDeck::AttentionMode::RealOptions) {
      hintFor(h2, sizeof(h2), optCount > 0 ? optLabels[0] : "", 2);
      hintFor(h3, sizeof(h3), optCount > 1 ? optLabels[1] : "", 3);
      hintFor(h4, sizeof(h4), optCount > 2 ? optLabels[2] : "", 4);
      GUI.drawButtonHints(renderer, "Later", h2, h3, h4);
    } else {
      GUI.drawButtonHints(renderer, "Later", "Detail", "", "");
    }
  } else {
    const auto labels = mappedInput.mapLabels("Later", "Select", "Up", "Down");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer();
}

void PocketDailyActivity::renderGlance(GlanceReason reason) {
  const bool isSleep = reason != GlanceReason::Ambient;
  const auto& m = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();
  const int pad = m.contentSidePadding;
  const int line12 = renderer.getLineHeight(UI_12_FONT_ID);
  const int line10 = renderer.getLineHeight(UI_10_FONT_ID);
  const int lineS = renderer.getLineHeight(SMALL_FONT_ID);

  // One bounded snapshot feeds the whole retained frame. This includes the
  // first carried Pocket item so a useful daily study prompt survives offline
  // and remains visible while the panel is asleep.
  preparePersonalSnapshot();
  PocketDaily::Glance& g = renderGlanceSnapshot;
  char syncedHm[6] = {0};
  snprintf(syncedHm, sizeof(syncedHm), "%s", renderSyncedHm);

  renderer.clearScreen();
  // Sleep has no interactive connection state; its immutable sync time stays
  // in the bottom line. Ambient uses the snapshot's date/time instead of a
  // clock that would become false on a retained e-ink frame.
  char glanceHeaderMeta[40] = {0};
  char snapshotDate[8] = {0};
  int metaChars = 0;
  // A powered-off frame may remain unchanged for days. Date and Sync time add
  // little value there and eventually become misleading, so reserve metadata
  // for the automatic cadence snapshot only.
  if (reason != GlanceReason::PoweredOff) {
    if (formatWeatherSnapshotDate(snapshotDate, sizeof(snapshotDate), g.weather))
      metaChars = snprintf(glanceHeaderMeta, sizeof(glanceHeaderMeta), "%s", snapshotDate);
    if (syncedHm[0] && metaChars < (int)sizeof(glanceHeaderMeta))
      snprintf(glanceHeaderMeta + metaChars, sizeof(glanceHeaderMeta) - metaChars, "%s%s %s",
               metaChars ? " \xC2\xB7 " : "", snapshotIsStale(renderSavedEpoch) ? "SAVED" : "SYNC", syncedHm);
  }
  drawBrandedHeader(tr(STR_POCKET_DAILY), glanceHeaderMeta[0] ? glanceHeaderMeta : nullptr);

  char buf[96];

  // ── Layout ──
  // The face is a set of independent sections that simply drop out when their
  // data is absent (no calendar → no TODAY, no daemon → no AI BUDGET, …).
  // Landscape panels (X4, 800×480) split into two columns — left: the personal
  // plane (READING / weather / TODAY), right: the work plane (AI BUDGET /
  // WORK) — so the wide screen reads as a dashboard, not a list. Portrait
  // (X3) keeps the single-column flow in the same section order. Every
  // section leads with a small labeled overline rule, so whatever subset of
  // data exists still composes into a designed page.
  const int topY = m.topPadding + m.headerHeight + m.verticalSpacing;
  const int statusY = pageH - lineS - 12;

  // "LABEL ────" overline. CJK-capable: the weather section uses the
  // user-supplied place name as its label. Returns the content start y.
  auto sectionHeader = [&](int x, int y, int cw, const char* label) -> int {
    const int f = fontForText(SMALL_FONT_ID, label);
    renderer.drawText(f, x, y, label, true, EpdFontFamily::BOLD);
    const int lw = renderer.getTextWidth(f, label, EpdFontFamily::BOLD);
    const int ly = y + lineS / 2 + 2;
    if (lw + 10 < cw) renderer.drawLine(x + lw + 10, ly, x + cw, ly);
    return y + lineS + 6;
  };

  // ── READING (local plane: the open book). Device-owned data — valid with
  // no daemon, no network, and no cached deck, which is what makes the glance
  // meaningful on a fully offline device. ──
  auto drawReading = [&](int x, int y, int cw) -> int {
    if (!renderReadingSnapshot.valid) return y;
    y = sectionHeader(x, y, cw, tr(STR_POCKET_CONTINUE_READING));

    // The retained face gives the current book a real visual identity. The
    // setting is intentionally cover-only: turning it off keeps resume/title
    // information useful while avoiding artwork on a desk or bedside panel.
    if (isSleep && SETTINGS.pocketDailySleepCover) {
      const bool portrait = pageH > w;
      const int coverW =
          portrait ? std::min(286, std::max(184, cw * 56 / 100)) : std::min(160, std::max(108, cw * 43 / 100));
      const int coverH = coverW * 3 / 2;
      drawReadingCover(x, y, coverW, coverH);

      const int metaX = x + coverW;
      renderer.drawLine(metaX, y, metaX, y + coverH, 2, true);
      renderer.drawLine(metaX, y, x + cw, y);
      renderer.drawLine(metaX, y + coverH, x + cw, y + coverH);
      const int textX = metaX + 14;
      const int textW = x + cw - textX - 13;
      const int titleFont = fontForText(UI_12_FONT_ID, renderReadingSnapshot.title);
      const int titleAdvance = renderer.getLineHeight(titleFont) + 3;
      int ty = y + 15;
      ty += drawWrappedFixed(renderer, titleFont, textX, ty, renderReadingSnapshot.title, textW, 3, titleAdvance,
                             EpdFontFamily::BOLD) *
            titleAdvance;
      if (renderReadingSnapshot.author[0]) {
        ty += 5;
        const int authorFont = fontForText(SMALL_FONT_ID, renderReadingSnapshot.author);
        renderer.drawText(authorFont, textX, ty,
                          renderer.truncatedText(authorFont, renderReadingSnapshot.author, textW).c_str(), true);
      }

      if (renderReadingSnapshot.percent >= 0) {
        char pct[8];
        snprintf(pct, sizeof(pct), "%d%%", renderReadingSnapshot.percent);
        const int progressY = y + coverH - line12 - 23;
        renderer.drawText(UI_12_FONT_ID, textX, progressY, pct, true, EpdFontFamily::BOLD);
        const int barY = y + coverH - 10;
        renderer.drawLine(textX, barY, textX + textW, barY);
        const int fillW = textW * renderReadingSnapshot.percent / 100;
        if (fillW > 0) renderer.drawLine(textX, barY - 2, textX + fillW, barY - 2, 4, true);
      }
      return y + coverH + 17;
    }

    const int tf = fontForText(UI_12_FONT_ID, renderReadingSnapshot.title);
    int titleW = cw;
    if (renderReadingSnapshot.percent >= 0) {
      char pct[8];
      snprintf(pct, sizeof(pct), "%d%%", renderReadingSnapshot.percent);
      const int pw = renderer.getTextWidth(UI_12_FONT_ID, pct);
      renderer.drawText(UI_12_FONT_ID, x + cw - pw, y, pct, true);
      titleW = cw - pw - 8;
    }
    renderer.drawText(tf, x, y, renderer.truncatedText(tf, renderReadingSnapshot.title, titleW).c_str(), true,
                      EpdFontFamily::BOLD);
    y += line12 + 4;
    char sub[96];
    sub[0] = '\0';
    if (renderReadingSnapshot.author[0]) snprintf(sub, sizeof(sub), "%s", renderReadingSnapshot.author);
    if (sub[0]) {
      const int sf = fontForText(SMALL_FONT_ID, sub);
      renderer.drawText(sf, x, y, renderer.truncatedText(sf, sub, cw).c_str(), true);
      y += lineS + 4;
    }
    if (renderReadingSnapshot.percent >= 0) {
      renderer.drawRect(x, y + 2, cw, 7);
      const int fillW = (cw - 4) * renderReadingSnapshot.percent / 100;
      if (fillW > 0) renderer.fillRect(x + 2, y + 4, fillW, 3);
      y += 13;
    }
    return y + 12;
  };

  // One carried learning/action item is useful even on a frozen panel. It is
  // deliberately read-only here: wake/open enters the normal Pocket card where
  // choices are durably queued before the item disappears.
  auto drawStudy = [&](int x, int y, int cw, int maxY) -> int {
    if (!renderPocketSnapshot.cardId[0] || y >= maxY) return y;
    y = sectionHeader(x, y, cw, tr(STR_POCKET_STUDY));
    const int titleFont = fontForText(UI_12_FONT_ID, renderPocketSnapshot.title);
    renderer.drawText(
        titleFont, x, y,
        renderer
            .truncatedText(titleFont, renderPocketSnapshot.title[0] ? renderPocketSnapshot.title : tr(STR_POCKET_STUDY),
                           cw, EpdFontFamily::BOLD)
            .c_str(),
        true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(titleFont) + 5;
    const int bodyFont = fontForText(UI_10_FONT_ID, renderPocketSnapshot.question);
    const int advance = renderer.getLineHeight(bodyFont) + 2;
    int maxLines = (maxY - y) / advance;
    if (maxLines > 3) maxLines = 3;
    if (maxLines > 0)
      y += drawWrappedFixed(renderer, bodyFont, x, y, renderPocketSnapshot.question, cw, maxLines, advance) * advance;
    return y + 12;
  };

  // ── Weather (the walking-out-the-door read; label = place name) ──
  auto drawWeather = [&](int x, int y, int cw, int maxY) -> int {
    if (!g.weather.valid) return y;
    char weatherLabel[56] = {0};
    snprintf(weatherLabel, sizeof(weatherLabel), "%s", g.weather.place[0] ? g.weather.place : tr(STR_POCKET_WEATHER));
    char snapshotDate[8] = {0};
    if (formatWeatherSnapshotDate(snapshotDate, sizeof(snapshotDate), g.weather))
      snprintf(weatherLabel + strlen(weatherLabel), sizeof(weatherLabel) - strlen(weatherLabel), " \xC2\xB7 %s%s",
               snapshotDate, snapshotIsStale(renderSavedEpoch) ? " SAVED" : "");
    y = sectionHeader(x, y, cw, weatherLabel);
    const int forecastRoom = maxY - y - 76;
    const int forecastH = g.weather.dayCount >= 2 && forecastRoom >= 106 ? std::min(148, forecastRoom) : 0;
    const int forecastY = maxY - forecastH;
    const int nowW = cw * 62 / 100;
    if (y + line12 < maxY) drawCurrentWeatherVisual(renderer, g.weather, x, y, nowW, std::max(46, forecastY - y - 4));
    int rainChars = AgentDeck::GlanceFormat::formatRainLine(buf, sizeof(buf), g.weather);
    if (rainChars == 0) rainChars = formatNextRainDay(buf, sizeof(buf), g.weather);
    if (rainChars > 0) {
      const int rainX = x + cw * 68 / 100;
      const int rainW = cw - (rainX - x);
      renderer.drawLine(rainX - 8, y, rainX - 8, std::max(y + 10, std::min(forecastY - 3, y + 66)), 3, true);
      const int rainFont = fontForText(UI_10_FONT_ID, buf);
      drawWrappedFixed(renderer, rainFont, rainX, y, buf, rainW, 2, line10 + 2, EpdFontFamily::BOLD);
    }
    if (forecastH > 0 && forecastY >= y + 48) {
      const int graphH = drawForecastRibbon(renderer, g.weather, x, forecastY, cw, forecastH);
      if (graphH > 0) return std::min(maxY, forecastY + graphH + 10);
    }
    return std::min(maxY, y + 74);
  };

  // ── TODAY (daemon-authored schedule, absolute HH:MM only). Absent when no
  // calendar is configured — the layout simply flows past it. ──
  auto drawToday = [&](int x, int y, int cw, int maxY) -> int {
    if (g.eventCount == 0) return y;
    y = sectionHeader(x, y, cw, tr(STR_POCKET_TODAY));
    for (uint8_t i = 0; i < g.eventCount; i++) {
      if (y + line10 >= maxY) break;
      if (AgentDeck::GlanceFormat::formatEventLine(buf, sizeof(buf), g.events[i]) <= 0) continue;
      const int f = fontForText(UI_10_FONT_ID, buf);
      renderer.drawText(f, x, y, renderer.truncatedText(f, buf, cw).c_str(), true);
      y += line10 + 4;
    }
    return y + 12;
  };

  // Pocket Glance is deliberately personal and locally meaningful: current
  // book, one carried study item, weather and today's schedule. Provider
  // quotas and live work/session summaries belong on AgentDeck dashboards.
  if (isSleep && pageH > w) {
    // The retained portrait is a glance, not a dashboard: the book and the
    // walking-out-the-door weather get first claim on space. Study appears as
    // a fallback when no book is open; calendar follows only if it still fits.
    int y = topY;
    y = drawReading(pad, y, w - pad * 2);
    if (!renderReadingSnapshot.valid) y = drawStudy(pad, y, w - pad * 2, statusY - 8);
    y = drawWeather(pad, y, w - pad * 2, statusY - 8);
    drawToday(pad, y, w - pad * 2, statusY - 8);
  } else if (isSleep) {
    // Wide retained panels keep the same priority as two calm columns.
    const int gap = 20;
    const int colW = (w - pad * 2 - gap) / 2;
    int leftY = drawReading(pad, topY, colW);
    if (!renderReadingSnapshot.valid) drawStudy(pad, leftY, colW, statusY - 8);
    int rightY = drawWeather(pad + colW + gap, topY, colW, statusY - 8);
    drawToday(pad + colW + gap, rightY, colW, statusY - 8);
  } else if (pageH > w) {
    int y = topY;
    y = drawReading(pad, y, w - pad * 2);
    y = drawStudy(pad, y, w - pad * 2, statusY - 8);
    y = drawWeather(pad, y, w - pad * 2, statusY - 8);
    drawToday(pad, y, w - pad * 2, statusY - 8);
  } else {
    const int gap = 20;
    const int colW = (w - pad * 2 - gap) / 2;
    int leftY = drawReading(pad, topY, colW);
    drawStudy(pad, leftY, colW, statusY - 8);
    int rightY = drawWeather(pad + colW + gap, topY, colW, statusY - 8);
    drawToday(pad + colW + gap, rightY, colW, statusY - 8);
  }

  // ── Bottom status: absolute times only — a retained frame must stay true
  // without a repaint, so never a relative age here. ──
  char status[112];
  if (pullOtaTotalBytes > 0 && pullOtaDownloadedBytes < pullOtaTotalBytes) {
    const unsigned pct = (unsigned)((uint64_t)pullOtaDownloadedBytes * 100 / pullOtaTotalBytes);
    if (pullOtaDownloading || manualSyncActive)
      snprintf(status, sizeof(status), "%s \xC2\xB7 %u%% \xC2\xB7 DOWNLOADING", tr(STR_POCKET_FIRMWARE), pct);
    else if (manualOtaResumePending)
      snprintf(status, sizeof(status), "%s \xC2\xB7 %u%% \xC2\xB7 AUTO RESUME", tr(STR_POCKET_FIRMWARE), pct);
    else
      snprintf(status, sizeof(status), "%s \xC2\xB7 %u%% \xC2\xB7 SYNC TO RESUME", tr(STR_POCKET_FIRMWARE), pct);
  } else
    switch (reason) {
      case GlanceReason::TimedSleep:
        if (syncedHm[0] && sleepNextHm[0])
          snprintf(status, sizeof(status), "%s %s \xC2\xB7 ~%s", tr(STR_POCKET_UPDATED), syncedHm, sleepNextHm);
        else if (pullSynced)
          snprintf(status, sizeof(status), "%s \xC2\xB7 ~%um", tr(STR_POCKET_SLEEPING), (unsigned)(sleepForSec / 60));
        else
          snprintf(status, sizeof(status), "%s \xC2\xB7 ~%um", tr(STR_POCKET_OFFLINE), (unsigned)(sleepForSec / 60));
        break;
      case GlanceReason::PoweredOff:
        // The physical wake tab communicates the important state. Do not repeat
        // a stale date or the last Sync time beside the front-button area.
        snprintf(status, sizeof(status), "%s", tr(STR_POCKET_POWERED_OFF));
        break;
      case GlanceReason::Ambient:
      default: {
        // Live face with nothing running: say where the data came from and how the
        // link stands, so the panel is honest without being an apology.
        const char* link = dashState == DashState::Connected     ? tr(STR_POCKET_UPDATED)
                           : dashState == DashState::Connecting  ? tr(STR_POCKET_CONNECTING)
                           : dashState == DashState::WifiJoining ? tr(STR_POCKET_CHECKING)
                           : dashState == DashState::Discovering ? tr(STR_POCKET_SEARCHING)
                                                                 : tr(STR_POCKET_OFFLINE);
        if (syncedHm[0])
          snprintf(status, sizeof(status), "%s \xC2\xB7 %s", link, syncedHm);
        else
          snprintf(status, sizeof(status), "%s", link);
        break;
      }
    }
  renderer.drawText(SMALL_FONT_ID, pad, statusY,
                    renderer.truncatedText(SMALL_FONT_ID, status, w - pad * 2, EpdFontFamily::BOLD).c_str(), true,
                    EpdFontFamily::BOLD);
  if (isSleep) PowerWakeCue::draw(renderer);

  if (isSleep) {
    // Ghost management: fast refreshes accumulate residue on a frame the panel
    // will hold for hours — insert a full waveform every Nth sleep paint.
    const bool fullClean = (timedSleepPaintSerial() % kGlanceFullRefreshEvery) == 0;
    renderer.displayBuffer(fullClean ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH);
  } else {
    // Confirm resumes the open book (M9 stage 1) — label it only when there is one.
    const auto labels = mappedInput.mapLabels(tr(STR_POCKET_LIBRARY),
                                              APP_STATE.openEpubPath.empty() ? "" : tr(STR_POCKET_READ), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
  }
}

bool PocketDailyActivity::paintSleepFrame() {
  // A manual power-off is personal, not a stale dashboard snapshot: release
  // ownership to SleepActivity, which shows the current cover full-screen or
  // the AgentDeck compatibility page when no book is open. Automatic cadence
  // sleep still calls renderGlance(TimedSleep) directly in beginTimedSleep().
  AgentLog::line("POCKET", "power-off frame delegated to cover sleep screen");
  return false;
}

void PocketDailyActivity::render(RenderLock&&) {
  // Assume a non-ambient face until the Ambient branch below proves otherwise;
  // every other path (sleep frame, OTA, Card/Detail, Overview) must not leave
  // Confirm bound to "resume reading".
  ambientGlanceShown = false;

  // Frozen sleep frame: a local Daily Brief painted once immediately before
  // timed deep sleep or power-off. It deliberately uses only persisted device
  // state (book position, carried study card, cached weather and schedule), so
  // the retained panel remains useful and truthful without a network.
  if (sleepFramePending) {
    renderGlance(glanceReason);
    return;
  }

  // Blocking OTA flash notice: painted once via requestUpdateAndWait() right
  // before the raw-partition write starts, so the panel holds this frame for
  // the ~1 minute of flashing and through the restart.
  if (otaFlashNotice) {
    const auto& mm = UITheme::getInstance().getMetrics();
    const int pad = mm.contentSidePadding;
    renderer.clearScreen();
    drawBrandedHeader("Firmware Update", nullptr);
    int y = mm.topPadding + mm.headerHeight + mm.verticalSpacing * 2;
    renderer.drawText(UI_12_FONT_ID, pad, y, tr(STR_POCKET_INSTALLING_UPDATE), true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_12_FONT_ID) + 10;
    renderer.drawText(UI_10_FONT_ID, pad, y, tr(STR_POCKET_DO_NOT_POWER_OFF), true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_10_FONT_ID) + 6;
    renderer.drawText(SMALL_FONT_ID, pad, y, tr(STR_POCKET_RESTART_WHEN_DONE), true);
    renderer.displayBuffer();
    return;
  }

  // Pocket cards are day-class and queue choices locally, so their Card view
  // remains valid offline. Session decisions / Detail still require Connected.
  if (viewMode == ViewMode::Card) {
    PocketDaily::Card pocket{};
    if (findPocketCard(cardSid, pocket)) {
      renderPocketCard(pocket);
      return;
    }
  }

  // Session Card (decision) / Detail (timeline) exist only while Connected.
  if (dashState == DashState::Connected) {
    if (viewMode == ViewMode::Card) {
      renderCard();
      return;
    }
    if (viewMode == ViewMode::Detail) {
      renderDetail();
      return;
    }
  }

  // ── Face: content-first shell in EVERY connection state. The Face renders
  // whatever is known (or an honest empty state); joining/discovering/connecting
  // progress is a status line inside renderOverview, never a screen that
  // replaces the content. ──
  OverviewRow* const rows = renderRows;
  int n = collectOverview(rows, kOverviewCap);

  // No live data yet (boot / daemon lost): append persisted Pocket cards after
  // the local Continue Reading row. Once any live feed arrives it wins, even
  // when empty; the cache must never mask fresher truth.
  bool fromCache = false;
  uint32_t asOfEpoch = 0;
  bool dataReceived;
  AgentDeck::lockState();
  dataReceived = AgentDeck::g_state.dataReceived;
  asOfEpoch = cachedDeck ? cachedDeck->savedEpoch : 0;
  AgentDeck::unlockState();
  fromCache = !dataReceived && cachedDeck && cachedDeck->pocketCount > 0;

  // With neither a book nor a Pocket item, personal glance remains useful as
  // an offline retained frame (reading/weather/today). It never outranks saved
  // items that can be opened and consumed.
  if (n == 0) {
    bool haveGlance = false;
    AgentDeck::lockState();
    haveGlance = AgentDeck::g_state.glance.valid || (cachedDeck && cachedDeck->glance.valid);
    AgentDeck::unlockState();
    // The local plane (open book) alone justifies the glance face: a device
    // that never met a daemon still shows the book + weatherless strip instead
    // of an empty deck apology.
    if (haveGlance || !APP_STATE.openEpubPath.empty()) {
      renderGlance(GlanceReason::Ambient);
      ambientGlanceShown = true;
      return;
    }
  }

  int awaiting = 0;
  if (!fromCache) {
    // Live only: a cached "awaiting" is a snapshot of the past, and the banner
    // is a call to action the user cannot take offline.
    for (int i = 0; i < n; i++)
      if (rows[i].awaiting) awaiting++;
  }
  renderOverview(rows, n, awaiting, fromCache, fromCache ? asOfEpoch : 0);
}
