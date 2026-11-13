#include "HomeRenderer.h"

#include <EpdFontFamily.h>
#include <GfxRenderer.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "fontIds.h"
#include "pocket_daily/home/GlanceFormat.h"

namespace PocketDaily::Home {
using OverviewRow = Row;

// Body moved from PocketDailyActivity::renderOverview (P1-3); the activity now
// builds a HomeView and calls this. Keep device and host behaviour identical.
int homeRowSources(const DailyProfile::Profile& profile, const RowAvailability& available,
                   RowSource (&out)[DailyProfile::HOME_ITEM_CAP]) {
  using DailyProfile::HomeItem;
  int n = 0;
  for (uint8_t k = 0; k < profile.homeCount && k < DailyProfile::HOME_ITEM_CAP; ++k) {
    switch (profile.homeItems[k]) {
      case HomeItem::Reading:
        // An empty reading slot is still the first usable route to the library.
        out[n++] = RowSource::Reading;
        break;
      case HomeItem::ReadingShelf:
      case HomeItem::ReadingList:
        out[n++] = RowSource::RecentBooks;
        break;
      case HomeItem::Calendar:
        out[n++] = RowSource::Calendar;
        break;
      case HomeItem::Study:
        // Without its own Word item, v1 keeps the daily word as the fallback.
        if (available.appCards)
          out[n++] = RowSource::AppCards;
        else if (profile.dailyWord && !profile.shows(HomeItem::Word))
          out[n++] = RowSource::DailyWord;
        break;
      case HomeItem::Word:
        out[n++] = RowSource::DailyWord;
        break;
      case HomeItem::ClockEvent:
      case HomeItem::ClockWeather:
      case HomeItem::ClockLine:
      case HomeItem::ClockLandscape:
        out[n++] = RowSource::DeskClock;
        break;
      case HomeItem::ClockCompact:
        break;
      case HomeItem::Provider:
      case HomeItem::Monitor:
        // Retired with the AgentDeck daemon: still valid in stored profiles,
        // never shown (docs/pocket-profile-v1.md).
        break;
    }
  }
  return n;
}

const char* cardRowTitle(const PocketDaily::Card& card) { return card.title[0] ? card.title : "POCKET"; }

// Moved from PocketDailyActivity::collectOverview so the Sync screen presenter
// composes the same Home row text.
void cardRowActivity(const PocketDaily::Card& card, char* activity, const size_t activityCap) {
  if (!activity || activityCap == 0) return;
  strncpy(activity, card.question, activityCap - 1);
  activity[activityCap - 1] = '\0';
  // The local SD lesson is a recall prompt on Home: show only the target
  // glyph there. Opening it reveals the word, reading, meaning and example.
  // The companion's cards keep their text + context overview summary.
  if (strcmp(card.module, "local") != 0 && card.context[0] && strcmp(activity, card.context) != 0) {
    const size_t used = strlen(activity);
    const size_t extra = (used ? 3 : 0) + strlen(card.context);
    // Keep UTF-8 intact; the detail view still carries the complete context
    // if the compact home row cannot fit it.
    if (used + extra < activityCap)
      snprintf(activity + used, activityCap - used, "%s%s", used ? " - " : "", card.context);
  }
}

void briefWeatherLabel(char* out, const size_t cap, const PocketDaily::Weather& weather, const char* fallback,
                       const bool stale) {
  if (!out || cap == 0) return;
  snprintf(out, cap, "%s", weather.place[0] ? weather.place : fallback);
  char snapshotDate[8] = {0};
  if (HomeDraw::formatWeatherSnapshotDate(snapshotDate, sizeof(snapshotDate), weather))
    snprintf(out + strlen(out), cap - strlen(out), " \xC2\xB7 %s%s", snapshotDate, stale ? " SAVED" : "");
}

// A typeset stand-in for a book without cover art: the title and author on a
// framed card with a spine. Real artwork is never invented.
void drawTitleCover(const GfxRenderer& renderer, const Env& env, const Reading& reading, const int x, const int y,
                    const int width, const int height) {
  if (width <= 24 || height <= 36) return;
  renderer.drawRect(x, y, width, height, 2, true);
  renderer.drawLine(x + 7, y, x + 7, y + height, 3, true);
  const int innerX = x + 20;
  const int innerW = width - 32;
  const int titleFont = env.text.resolve(reading.title, UI_12_FONT_ID);
  const int titleAdvance = renderer.getLineHeight(titleFont) + 3;
  const int authorFont = env.text.resolve(reading.author, SMALL_FONT_ID);
  const int authorH = reading.author[0] ? renderer.getLineHeight(authorFont) + 12 : 0;
  const int maxLines = std::max(1, std::min(6, (height - 32 - authorH) / titleAdvance));
  const auto lines = renderer.wrappedText(titleFont, reading.title, innerW, maxLines, EpdFontFamily::BOLD);
  int ty = y + std::max(16, (height - static_cast<int>(lines.size()) * titleAdvance - authorH) / 2);
  for (const auto& line : lines) {
    renderer.drawText(titleFont, innerX, ty, line.c_str(), true, EpdFontFamily::BOLD);
    ty += titleAdvance;
  }
  if (reading.author[0]) {
    renderer.drawLine(innerX, ty + 4, innerX + std::min(innerW, 48), ty + 4, 2, true);
    renderer.drawText(authorFont, innerX, ty + 12, renderer.truncatedText(authorFont, reading.author, innerW).c_str(),
                      true);
  }
}

ClockDetail clockDetail(const DailyProfile::Profile& profile, const DeskClock& clock, const Glance& glance,
                        const Strings& s) {
  using DailyProfile::HomeItem;
  switch (profile.clockPage()) {
    case HomeItem::ClockLandscape:
    case HomeItem::ClockEvent: {
      ClockDetail detail{s.nextEvent};
      const Event* next = nullptr;
      int earliest = 1440;
      if (clock.minute < 1440) {
        for (uint8_t i = 0; i < glance.eventCount && i < Glance::EVENT_CAP; ++i) {
          const int start = GlanceFormat::hmToMinutes(glance.events[i].startHm);
          if (start < clock.minute || start >= earliest || !glance.events[i].title[0]) continue;
          earliest = start;
          next = &glance.events[i];
        }
      }
      if (next)
        snprintf(detail.text, sizeof(detail.text), "%s %s  ·  %dm", next->startHm, next->title,
                 earliest - clock.minute);
      else
        snprintf(detail.text, sizeof(detail.text), "%s", s.noEvents);
      return detail;
    }
    case HomeItem::ClockWeather: {
      ClockDetail detail{s.weather};
      if (!glance.weather.valid)
        snprintf(detail.text, sizeof(detail.text), "%s", s.noWeather);
      else if (!GlanceFormat::formatRainLine(detail.text, sizeof(detail.text), glance.weather))
        GlanceFormat::formatWeatherNow(detail.text, sizeof(detail.text), glance.weather);
      return detail;
    }
    default: {
      ClockDetail detail{s.today};
      snprintf(detail.text, sizeof(detail.text), "%s", clock.line);
      return detail;
    }
  }
}

namespace {
// Compact monochrome numeral masks. Run lengths draw as rectangles at the
// same scale on both panels, independent of loaded fonts or image caches.
constexpr uint8_t kDigits[10][7] = {
    {14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14},    {14, 17, 1, 2, 4, 8, 31},     {30, 1, 1, 14, 1, 1, 30},
    {2, 6, 10, 18, 31, 2, 2},     {31, 16, 16, 30, 1, 1, 30}, {14, 16, 16, 30, 17, 17, 14}, {31, 1, 2, 4, 8, 8, 8},
    {14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 15, 1, 1, 14}};

void drawDeskClock(GfxRenderer& r, const HomeView& view, const Env& env, const int headerHeight) {
  const int w = r.getScreenWidth();
  const int h = r.getScreenHeight();
  const int pad = env.metrics.contentSidePadding;
  const int digitCount = view.clock.second < 60 ? 6 : 4;
  const int widthInUnits = digitCount * 10 - 3;
  const int unit = std::max(1, std::min((w - 2 * pad) / widthInUnits, h / (w > h ? 31 : 25)));
  const int tile = 8 * unit;
  const int top = w > h ? h / 5 : h * 3 / 10;
  const int left = (w - widthInUnits * unit) / 2;
  const bool valid = view.clock.minute < 1440;
  const int hour = view.clock.minute / 60;
  const int minute = view.clock.minute % 60;
  const int second = view.clock.second;
  const int numbers[] = {hour / 10, hour % 10, minute / 10, minute % 10, second / 10, second % 10};
  const int priorHour = view.clock.foldingFromMinute / 60;
  const int priorMinute = view.clock.foldingFromMinute % 60;
  const int priorSecond = view.clock.foldingFromSecond;
  const int prior[] = {priorHour / 10,   priorHour % 10,   priorMinute / 10,
                       priorMinute % 10, priorSecond / 10, priorSecond % 10};

  r.clearScreen();
  if (env.drawHeader)
    env.drawHeader(env.context, r, 0, env.metrics.topPadding, w, headerHeight, view.strings.pocketDaily, nullptr);
  for (int index = 0; index < digitCount; ++index) {
    const int x = left + (index * 9 + index / 2 * 2) * unit;
    const bool hasPrevious = index < 4 ? view.clock.foldingFromMinute < 1440 : priorSecond < 60;
    const bool folding = hasPrevious && prior[index] != numbers[index];
    const int glyphX = x + (tile - 5 * unit) / 2;
    const int glyphY = top + 2 * unit;
    const int hinge = top + 11 * unit / 2;
    r.fillRect(x, top, tile, 11 * unit);
    if (valid) {
      for (int row = 0; row < 7; ++row) {
        // The new upper leaf is exposed first. The lower leaf still carries
        // the previous digit until the moving flap settles.
        const int numeral = folding && row >= 3 ? prior[index] : numbers[index];
        for (int col = 0; col < 5; ++col)
          if (kDigits[numeral][row] & (1u << (4 - col)))
            r.fillRect(glyphX + col * unit, glyphY + row * unit, unit, unit, false);
      }
    } else {
      r.fillRect(glyphX + unit, glyphY + 3 * unit, 3 * unit, unit, false);
    }
    if (folding) {
      const int flapDepth = std::max(2, unit / 2);
      r.fillRect(x + 1, hinge + 1, tile - 2, flapDepth);
      r.drawLine(x + unit, hinge + flapDepth, x + tile - unit - 1, hinge + flapDepth, false);
    }
    r.fillRect(x, hinge, tile, 1, false);
  }
  if (!view.clock.hideColon) {
    for (int group = 1; group < digitCount / 2; ++group) {
      const int center = left + (group * 20 - 2) * unit;
      r.fillRect(center, top + 4 * unit, unit, unit);
      r.fillRect(center, top + 7 * unit, unit, unit);
    }
  }

  char calendar[18]{};
  const unsigned value = view.clock.date;
  if (valid && value >= 19700101 && value <= 99991231) {
    char date[11]{}, weekday[4]{};
    snprintf(date, sizeof(date), "%04u-%02u-%02u", value / 10000, (value / 100) % 100, value % 100);
    GlanceFormat::formatWeekday(weekday, sizeof(weekday), date);
    snprintf(calendar, sizeof(calendar), "%s  %s", date, weekday);
  }
  const int dateY = top + 13 * unit;
  r.drawCenteredText(UI_12_FONT_ID, dateY, calendar, true, EpdFontFamily::BOLD);
  const int dividerY = dateY + r.getLineHeight(UI_12_FONT_ID) + 2 * unit;
  r.drawLine(pad, dividerY, w - pad, dividerY);
  const auto detail = clockDetail(view.profile, view.clock, *view.glance, view.strings);
  const int labelY = dividerY + unit;
  r.drawCenteredText(env.text.resolve(detail.label, SMALL_FONT_ID), labelY, detail.label);
  const int body = env.text.resolve(detail.text, UI_12_FONT_ID);
  const auto fitted = r.truncatedText(body, detail.text, w - 2 * pad);
  r.drawCenteredText(body, labelY + r.getLineHeight(SMALL_FONT_ID) + unit / 2, fitted.c_str());
  const auto& s = view.strings;

  HomeDraw::drawPocketActionStrip(r, view.isX3, env.labels, s.home, view.reading.valid ? s.select : s.library, s.views,
                                  s.sync);
}
}  // namespace

int stepHomeSection(const uint8_t* groups, int count, int selected, int direction) {
  if (!groups || count <= 0) return 0;
  if (selected < 0 || selected >= count) selected = 0;
  for (int distance = 1; distance <= count; ++distance) {
    const int candidate = (selected + (direction < 0 ? -distance : distance) + count) % count;
    if (groups[candidate] == groups[selected]) return candidate;
  }
  return selected;
}
int homeSectionStarts(const uint8_t* groups, int count, uint8_t* starts, int capacity) {
  int found = 0;
  for (int i = 0; groups && starts && i < count && found < capacity; ++i) {
    bool seen = false;
    for (int j = 0; j < found; ++j)
      if (groups[starts[j]] == groups[i]) seen = true;
    if (!seen) starts[found++] = i;
  }
  return found;
}

int calendarDays(const int year, const int month) {
  static constexpr int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (year < 1 || year > 9999 || month < 1 || month > 12) return 0;
  return days[month - 1] + (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}
int calendarWeekday(int year, const int month) {
  if (!calendarDays(year, month)) return -1;
  static constexpr int shift[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  year -= month < 3;
  return (year + year / 4 - year / 100 + year / 400 + shift[month - 1] + 7) % 7;
}
uint32_t calendarMonth(const uint32_t date, const int offset) {
  const int year = date / 10000, month = date / 100 % 100;
  if (!calendarDays(year, month) || date % 100 < 1 || date % 100 > static_cast<unsigned>(calendarDays(year, month)))
    return 0;
  const int index = year * 12 + month - 1 + std::max(-12, std::min(12, offset));
  if (index < 12 || index >= 10000 * 12) return 0;
  return (index / 12) * 10000u + (index % 12 + 1) * 100u + 1;
}
namespace {
void drawRecentBooks(GfxRenderer& r, const HomeView& view, const Env& env, const int x, const int y, const int width,
                     const int height, const int selected) {
  const int lineH = r.getLineHeight(UI_12_FONT_ID);
  const int smallH = r.getLineHeight(SMALL_FONT_ID);
  const int count = view.recentBooks ? std::min<int>(view.recentCount, RECENT_CAP) : 0;

  if (!count) {
    const int font = env.text.resolve(view.strings.startReading, UI_12_FONT_ID);
    HomeDraw::drawWrappedFixed(r, font, x, y + lineH * 2, view.strings.startReading, width, 3,
                               r.getLineHeight(font) + 4);
    return;
  }
  // A wide shelf gives each cover one full-height column; a two-row grid
  // would consume the image area with repeated title/progress chrome.
  const bool wide = width > height && width >= count * 150 && height > lineH * 8;
  const bool tiles =
      view.profile.shows(DailyProfile::HomeItem::ReadingShelf) && (wide || (height > lineH * 13 && width > 300));
  const int gap = std::max(8, env.metrics.verticalSpacing);
  const int top = y;
  const int columns = tiles ? (wide ? count : 2) : 1;
  const int rows = (count + columns - 1) / columns;
  const int cellW = (width - (columns - 1) * gap) / columns;
  const int cellH = std::min((height - (rows - 1) * gap) / rows, lineH * 2 + smallH * 2 + 24);
  for (int i = 0; i < count; ++i) {
    const auto& book = view.recentBooks[i];
    const int bx = x + (i % columns) * (cellW + gap), by = top + (i / columns) * (cellH + gap);
    const int textX = bx + 14, textW = cellW - 28;
    r.drawLine(bx + 12, by + cellH, bx + cellW, by + cellH, 1, true);
    if (i == selected) r.fillRect(bx + 1, by + 8, 4, cellH - 16, true);
    const int font = env.text.resolve(book.title, UI_12_FONT_ID);
    const int advance = r.getLineHeight(font) + 3;
    int ty = by + 12;
    const int coverH = tiles ? cellH - advance * 2 - smallH * 2 - 36 : 0;
    const bool hasCover =
        coverH >= lineH * 2 && env.drawRecentCover && env.drawRecentCover(env.context, r, i, textX, ty, textW, coverH);
    if (hasCover) ty += coverH + 6;
    const int titleLines = std::max(1, std::min(hasCover ? 2
                                                : tiles  ? 4
                                                         : 2,
                                                (by + cellH - ty - smallH * 2 - 16) / advance));
    ty += HomeDraw::drawWrappedFixed(r, font, textX, ty, book.title, textW, titleLines, advance, EpdFontFamily::BOLD) *
          advance;
    const int author = env.text.resolve(book.author, SMALL_FONT_ID);
    if (ty + r.getLineHeight(author) + smallH + 10 < by + cellH)
      HomeDraw::drawWrappedFixed(r, author, textX, ty + 4, book.author, textW, 1, r.getLineHeight(author));
    if (book.percent >= 0 && cellH >= lineH + smallH + 24) {
      char percent[8];
      snprintf(percent, sizeof(percent), "%d%%", book.percent);
      const int py = by + cellH - smallH - 8;
      r.drawText(SMALL_FONT_ID, textX + textW - r.getTextWidth(SMALL_FONT_ID, percent), py, percent, true,
                 EpdFontFamily::BOLD);
      const int barW = std::max(1, textW - r.getTextWidth(SMALL_FONT_ID, "100%") - 12);
      r.drawLine(textX, py + smallH / 2, textX + barW, py + smallH / 2, 1, true);
      if (book.percent) r.drawLine(textX, py + smallH / 2, textX + barW * book.percent / 100, py + smallH / 2, 5, true);
    }
  }
}
void drawCalendar(const GfxRenderer& r, const HomeView& view, const Env& env, const int x, const int y, const int width,
                  const int height) {
  const uint32_t selectedDate = view.calendarSelection ? view.calendarSelection : view.clock.date;
  const auto date = calendarMonth(selectedDate, view.calendarOffset);
  const bool split = width > height * 14 / 10;
  const int gridWidth = split ? width * 58 / 100 : width;
  const int detailsX = split ? x + gridWidth + 18 : x;
  const int detailsWidth = split ? width - gridWidth - 18 : width;
  const int heading = env.labels.resolve(view.strings.calendar, UI_12_FONT_ID);
  r.drawText(heading, x, y, view.strings.calendar, true, EpdFontFamily::BOLD);
  if (!date) {
    r.drawText(UI_12_FONT_ID, x, y + r.getLineHeight(heading) * 2, "---- / --", true);
    return;
  }
  const int year = date / 10000, month = date / 100 % 100;
  char label[16];
  snprintf(label, sizeof(label), "%04d / %02d", year, month);
  const int lineH = r.getLineHeight(UI_12_FONT_ID), smallH = r.getLineHeight(SMALL_FONT_ID);
  const int labelX = x + gridWidth - r.getTextWidth(UI_12_FONT_ID, label, EpdFontFamily::BOLD);
  if (view.calendarBrowsing) r.drawRect(labelX - 5, y - 5, x + gridWidth - labelX + 10, lineH + 10, 2, true);
  r.drawText(UI_12_FONT_ID, labelX, y, label, true, EpdFontFamily::BOLD);
  const int gridTop = y + lineH + 16;
  const int cellW = gridWidth / 7;
  const int cellH = std::max(lineH + 8, std::min(lineH + 22, (height - lineH * 5) / 7));
  const char* weekday = view.strings.weekdays;
  for (int i = 0; i < 7; ++i) {
    char name[24]{};
    size_t n = 0;
    while (weekday[n] && weekday[n] != '|' && n < sizeof(name) - 1) {
      name[n] = weekday[n];
      ++n;
    }
    const int font = env.labels.resolve(name, SMALL_FONT_ID);
    r.drawText(font, x + i * cellW + (cellW - r.getTextWidth(font, name)) / 2, gridTop, name, true);
    weekday += n;
    if (*weekday == '|') ++weekday;
  }
  const int first = calendarWeekday(year, month), days = calendarDays(year, month);
  for (int d = 1; d <= days; ++d) {
    const int index = first + d - 1, bx = x + (index % 7) * cellW, by = gridTop + smallH + 8 + (index / 7) * cellH;
    const bool today = view.calendarOffset == 0 && static_cast<unsigned>(d) == selectedDate % 100;
    if (today) r.fillRect(bx + 2, by - 3, cellW - 4, lineH + 6, true);
    char number[4];
    snprintf(number, sizeof(number), "%d", d);
    r.drawText(UI_12_FONT_ID, bx + (cellW - r.getTextWidth(UI_12_FONT_ID, number)) / 2, by, number, !today,
               EpdFontFamily::BOLD);
    const auto* agendaDay = view.agenda ? Agenda::findDay(*view.agenda, date + d - 1) : nullptr;
    if (agendaDay && (agendaDay->eventCount || agendaDay->omittedEventCount))
      r.fillRect(bx + cellW / 2 - 2, by + lineH + 4, 4, 3, true);
  }
  const int weeks = (first + days + 6) / 7;
  int ey = split ? gridTop : gridTop + smallH + 8 + weeks * cellH + 12;
  if (ey + smallH + lineH + 18 > y + height) return;
  r.drawLine(detailsX, ey, detailsX + detailsWidth, ey, 2, true);
  ey += 10;
  char selectedLabel[16];
  snprintf(selectedLabel, sizeof(selectedLabel), "%04u/%02u/%02u", unsigned(selectedDate / 10000),
           unsigned(selectedDate / 100 % 100), unsigned(selectedDate % 100));
  r.drawText(SMALL_FONT_ID, detailsX, ey, selectedLabel, true, EpdFontFamily::BOLD);
  ey += smallH + 8;
  const auto* day = view.agenda ? Agenda::findDay(*view.agenda, selectedDate) : nullptr;
  const bool legacyToday = !view.agenda && selectedDate == view.clock.date;
  const auto* events = day ? day->events : legacyToday ? view.glance->events : nullptr;
  const uint8_t count = day ? day->eventCount : legacyToday ? view.glance->eventCount : 0;
  if (!events || !count) {
    // A dash denotes a date not downloaded, not an assertion that it is free.
    const char* empty = events ? view.strings.noEvents : "-";
    const int font = env.text.resolve(empty, UI_12_FONT_ID);
    HomeDraw::drawWrappedFixed(r, font, detailsX, ey, empty, detailsWidth, 1, r.getLineHeight(font));
  }
  uint8_t drawn = 0;
  for (; drawn < count && drawn < Glance::EVENT_CAP && ey + lineH <= y + height; ++drawn) {
    char event[96];
    GlanceFormat::formatEventLine(event, sizeof(event), events[drawn]);
    const int font = env.text.resolve(event, UI_12_FONT_ID);
    HomeDraw::drawWrappedFixed(r, font, detailsX, ey, event, detailsWidth, 1, r.getLineHeight(font));
    ey += r.getLineHeight(font) + 10;
  }
  const unsigned omitted = (day ? day->omittedEventCount : 0) + count - drawn;
  if (omitted) {
    char more[12];
    snprintf(more, sizeof(more), "+%u", omitted);
    r.drawText(SMALL_FONT_ID, detailsX + detailsWidth - r.getTextWidth(SMALL_FONT_ID, more),
               (split ? gridTop : gridTop + smallH + 8 + weeks * cellH + 12) + 10, more, true);
  }
}
}  // namespace

void renderHome(GfxRenderer& renderer, const HomeView& view, const Env& env) {
  const auto& m = env.metrics;
  const auto& s = view.strings;
  const OverviewRow* rows = view.rows;
  const int n = view.count;
  const int w = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();
  const int cursor = view.selected >= 0 && view.selected < n ? view.selected : 0;
  const bool newInterface = n > 0 && (rows[cursor].shelfIndex >= 0 || rows[cursor].calendarPage);
  uint8_t groups[16]{};
  const int boundedCount = std::min(n, 16);
  int groupCount = 0, groupPosition = 0;
  for (int i = 0; i < boundedCount; ++i) {
    groups[i] = static_cast<uint8_t>(
        homeSection(rows[i].reading, rows[i].shelfIndex >= 0, rows[i].clockPage, rows[i].calendarPage));
    if (groups[i] == static_cast<uint8_t>(homeSection(rows[cursor].reading, rows[cursor].shelfIndex >= 0,
                                                      rows[cursor].clockPage, rows[cursor].calendarPage))) {
      ++groupCount;
      if (i <= cursor) ++groupPosition;
    }
  }
  const bool sidePaging = groupCount > 1 || view.calendarBrowsing || view.choosingView;
  const bool rotatedNew = newInterface && renderer.getOrientation() != GfxRenderer::Orientation::Portrait;
  const int hardwareMargin = rotatedNew ? renderer.getLineHeight(SMALL_FONT_ID) + 20 : 0;
  // X3 gets borderless edge chevrons instead of the old 30px PREV/NEXT rails.
  // X4 has enough width to retain its hardware-aligned side hints.
  const int originalPad =
      sidePaging && !view.isX3 ? std::max(m.contentSidePadding, m.sideButtonHintsWidth + 6) : m.contentSidePadding;
  const int pad = std::max(originalPad, hardwareMargin);
  const int headerY =
      m.topPadding +
      (rotatedNew && renderer.getOrientation() == GfxRenderer::Orientation::PortraitInverted ? hardwareMargin : 0);
  const int line12 = renderer.getLineHeight(UI_12_FONT_ID);
  const int line10 = renderer.getLineHeight(UI_10_FONT_ID);
  // Local names match the activity members this body was moved from.
  const PocketDaily::Glance& renderGlanceSnapshot = *view.glance;
  Reading renderReadingSnapshot = view.reading;
  const int selectedShelf = n > 0 ? rows[cursor].shelfIndex : -1;
  if (selectedShelf >= 0 && view.recentBooks && selectedShelf < view.recentCount) {
    const auto& book = view.recentBooks[selectedShelf];
    renderReadingSnapshot = {book.valid, book.title, book.author, book.percent};
  }
  auto fontForText = [&](int uiFontId, const char* text) { return env.text.resolve(text, uiFontId); };
  auto drawReadingCover = [&](int x, int y, int width, int height) {
    if (selectedShelf >= 0 && env.drawRecentCover &&
        env.drawRecentCover(env.context, renderer, selectedShelf, x, y, width, height))
      return true;
    if (selectedShelf < 0 && env.drawCover && env.drawCover(env.context, renderer, x, y, width, height)) return true;
    drawTitleCover(renderer, env, renderReadingSnapshot, x, y, width, height);
    return false;
  };
  using HomeDraw::drawForecastGrid;
  using HomeDraw::drawWeatherPlaceholder;
  using HomeDraw::drawWeatherPoster;
  using HomeDraw::drawWrappedFixed;
  using HomeDraw::formatWeatherSnapshotDate;

  if (view.choosingView) {
    renderer.clearScreen();
    uint8_t starts[4]{};
    const int sections = homeSectionStarts(groups, boundedCount, starts, 4);
    int y = headerY + 12;
    renderer.drawText(UI_12_FONT_ID, pad, y, s.views, true, EpdFontFamily::BOLD);
    y += line12 + 20;
    for (int i = 0; i <= sections; ++i) {
      const Section section = i < sections ? static_cast<Section>(groups[starts[i]]) : Section::Reading;
      const char* label = i == sections                  ? s.articles
                          : section == Section::Reading  ? s.reading
                          : section == Section::Calendar ? s.calendar
                          : section == Section::Clock    ? s.clock
                                                         : s.study;
      const int font = env.labels.resolve(label, UI_12_FONT_ID);
      const int h = renderer.getLineHeight(font) + 18;
      if (i == view.viewChoice) renderer.drawRect(pad - 4, y - 7, w - pad * 2 + 8, h, 2, true);
      renderer.drawText(font, pad + 10, y, label, true, EpdFontFamily::BOLD);
      y += h + 6;
    }
    HomeDraw::drawPocketActionStrip(renderer, view.isX3, env.labels, s.back, s.select, s.views, s.sync);
    HomeDraw::drawPocketSideChevrons(renderer, view.isX3);
    return;
  }
  const bool readingPage = n > 0 && (rows[cursor].reading || selectedShelf >= 0);
  const int headerHeight = readingPage && view.profile.shows(DailyProfile::HomeItem::ClockCompact)
                               ? 0
                               : std::min(m.headerHeight, line12 + (readingPage ? 4 : m.verticalSpacing * 2));
  const int selectedIndex = view.selected >= 0 && view.selected < n ? view.selected : 0;
  if (view.profile.deskClock() || (n > 0 && rows[selectedIndex].clockPage)) {
    const auto orientation = renderer.getOrientation();
    if (view.profile.clockPage() == DailyProfile::HomeItem::ClockLandscape)
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
    drawDeskClock(renderer, view, env, headerHeight);
    renderer.setOrientation(orientation);
    return;
  }
  renderer.clearScreen();
  if (env.drawHeader && headerHeight > 0)
    env.drawHeader(env.context, renderer, pad - originalPad, headerY, w - 2 * (pad - originalPad), headerHeight,
                   s.pocketDaily, nullptr);
  const int contentTop = headerY + headerHeight + m.verticalSpacing;
  const int hintTop = pageH - std::max(renderer.getLineHeight(SMALL_FONT_ID) + 16, hardwareMargin);
  const bool portrait = pageH > w;

  // The row list is also the visible carousel: the current book and carried
  // study items use the same large content area instead of competing cards.
  const int selectedPocketIndex = n > 0 && rows[selectedIndex].pocket ? selectedIndex : -1;
  char carouselPosition[16] = {0};
  if (groupCount > 1) snprintf(carouselPosition, sizeof(carouselPosition), "%d/%d", groupPosition, groupCount);

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

  auto centeredIn = [&](int font, int x, int cw, int y, const char* text, EpdFontFamily::Style style) {
    const std::string fitted = renderer.truncatedText(font, text, cw, style);
    renderer.drawText(font, x + std::max(0, (cw - renderer.getTextWidth(font, fitted.c_str(), style)) / 2), y,
                      fitted.c_str(), true, style);
  };

  auto drawReading = [&](int x, int y, int cw, int ch) {
    const int panelBottom = y + ch;
    if (!renderReadingSnapshot.valid) {
      // The whole panel says what is missing and names the control below that fixes it.
      y = sectionHeader(x + 12, y + 10, cw - 24, s.continueReading, carouselPosition);
      const int emptyFont = fontForText(UI_12_FONT_ID, s.noOpenBook);
      const int hintFont = fontForText(UI_10_FONT_ID, s.startReading);
      const int mid = y + (panelBottom - y) / 2;
      centeredIn(emptyFont, x + 12, cw - 24, mid - renderer.getLineHeight(emptyFont) - 4, s.noOpenBook,
                 EpdFontFamily::BOLD);
      centeredIn(hintFont, x + 12, cw - 24, mid + 8, s.startReading, EpdFontFamily::REGULAR);
      return;
    }

    auto drawProgress = [&](int textX, int textW, int bottom) {
      if (renderReadingSnapshot.percent < 0) {
        if (carouselPosition[0])
          renderer.drawText(SMALL_FONT_ID, textX + textW - renderer.getTextWidth(SMALL_FONT_ID, carouselPosition),
                            bottom - line10 - 10, carouselPosition, true);
        return;
      }
      char pct[8];
      snprintf(pct, sizeof(pct), "%d%%", renderReadingSnapshot.percent);
      const int progressY = bottom - line12 - 24;
      renderer.drawText(UI_12_FONT_ID, textX, progressY, pct, true, EpdFontFamily::BOLD);
      const char* resume = carouselPosition;
      const int resumeW = renderer.getTextWidth(SMALL_FONT_ID, resume, EpdFontFamily::BOLD);
      renderer.drawText(SMALL_FONT_ID, textX + textW - resumeW, progressY + line12 - line10, resume, true,
                        EpdFontFamily::BOLD);
      const int barY = bottom - 10;
      renderer.drawLine(textX, barY, textX + textW, barY);
      const int fillW = textW * renderReadingSnapshot.percent / 100;
      if (fillW > 0) renderer.drawLine(textX, barY - 2, textX + fillW, barY - 2, 4, true);
    };

    if (ch > cw) {
      // A tall panel (portrait with the Daily panel off): the cover takes the
      // height above a full-width title, author and progress block.
      const int textX = x + 14;
      const int textW = cw - 28;
      int ty = y + 4;
      const int titleFont = fontForText(UI_12_FONT_ID, renderReadingSnapshot.title);
      const int titleAdvance = renderer.getLineHeight(titleFont) + 2;
      const int titleLines = HomeDraw::measureWrappedFixed(renderer, titleFont, renderReadingSnapshot.title, textW, 3,
                                                           EpdFontFamily::BOLD);
      const int authorFont = fontForText(SMALL_FONT_ID, renderReadingSnapshot.author);
      const int authorH = renderReadingSnapshot.author[0] ? renderer.getLineHeight(authorFont) + 6 : 0;
      const int progressH = renderReadingSnapshot.percent >= 0 ? line12 + 34 : 0;
      const int textBlock = titleAdvance * titleLines + authorH + progressH + 12;
      const int coverH = std::max(0, std::min(panelBottom - ty - textBlock, (textW - 12) * 3 / 2));
      const int coverW = coverH * 2 / 3;
      if (coverW >= 60) {
        drawReadingCover(x + (cw - coverW) / 2, ty, coverW, coverH);
        ty += coverH + 10;
      }
      ty += drawWrappedFixed(renderer, titleFont, textX, ty, renderReadingSnapshot.title, textW, 3, titleAdvance,
                             EpdFontFamily::BOLD) *
            titleAdvance;
      if (renderReadingSnapshot.author[0]) {
        renderer.drawText(authorFont, textX, ty + 4,
                          renderer.truncatedText(authorFont, renderReadingSnapshot.author, textW).c_str(), true);
      }
      drawProgress(textX, textW, panelBottom);
      return;
    }

    // A wide panel: cover column beside the title, author and progress.
    const int wideBottom = y + ch;
    const int coverColumn = cw * 47 / 100;
    const int coverH = std::min(ch - 24, (coverColumn - 24) * 3 / 2);
    const int coverW = coverH * 2 / 3;
    drawReadingCover(x + (coverColumn - coverW) / 2, y + (ch - coverH) / 2, coverW, coverH);
    const int metaX = x + coverColumn;
    renderer.drawLine(metaX, y, metaX, wideBottom, 2, true);
    const int textX = metaX + 14;
    const int textW = x + cw - textX - 13;
    const int titleFont = fontForText(UI_12_FONT_ID, renderReadingSnapshot.title);
    const int titleAdvance = renderer.getLineHeight(titleFont) + 2;
    const int authorFont = fontForText(SMALL_FONT_ID, renderReadingSnapshot.author);
    const int authorH = renderReadingSnapshot.author[0] ? renderer.getLineHeight(authorFont) + 5 : 0;
    const int textTop = y + 12;
    const int textBottom = wideBottom - (renderReadingSnapshot.percent >= 0 ? line12 + 36 : 14);
    const int maxTitleLines = std::max(1, std::min(4, (textBottom - textTop - authorH) / titleAdvance));
    const int titleLines = HomeDraw::measureWrappedFixed(renderer, titleFont, renderReadingSnapshot.title, textW,
                                                         maxTitleLines, EpdFontFamily::BOLD);
    int ty = textTop + std::max(0, (textBottom - textTop - titleLines * titleAdvance - authorH) / 2);
    ty += drawWrappedFixed(renderer, titleFont, textX, ty, renderReadingSnapshot.title, textW, maxTitleLines,
                           titleAdvance, EpdFontFamily::BOLD) *
          titleAdvance;
    if (renderReadingSnapshot.author[0]) {
      ty += 5;
      renderer.drawText(authorFont, textX, ty,
                        renderer.truncatedText(authorFont, renderReadingSnapshot.author, textW).c_str(), true);
    }
    drawProgress(textX, textW, wideBottom);
  };

  auto drawStudy = [&](int x, int y, int cw, int ch) {
    if (cw <= 16 || ch <= 16) return;
    const int panelBottom = y + ch;
    renderer.drawRect(x, y, cw, ch, 2, true);
    if (selectedPocketIndex < 0) {
      y = sectionHeader(x + 12, y + 10, cw - 24, s.study, nullptr);
      const int emptyFont = fontForText(UI_10_FONT_ID, s.empty);
      const int emptyAdvance = renderer.getLineHeight(emptyFont) + 3;
      const int emptyY = y + std::max(0, (panelBottom - y - emptyAdvance * 2) / 2);
      drawWrappedFixed(renderer, emptyFont, x + 12, emptyY, s.empty, cw - 24, 2, emptyAdvance);
      return;
    }
    const OverviewRow& row = rows[selectedPocketIndex];
    y = sectionHeader(x + 12, y + 10, cw - 24, row.mine ? s.myCards : row.word ? s.word : s.study, carouselPosition);
    const int titleFont = fontForText(UI_12_FONT_ID, row.project);
    renderer.drawText(titleFont, x + 12, y,
                      renderer.truncatedText(titleFont, row.project, cw - 24, EpdFontFamily::BOLD).c_str(), true,
                      EpdFontFamily::BOLD);
    y += renderer.getLineHeight(titleFont) + 8;
    if (y + 8 >= panelBottom) return;
    const int bodyFont = fontForText(UI_10_FONT_ID, row.activity);
    const int advance = renderer.getLineHeight(bodyFont) + 3;
    // A card image (a QR code, a map) takes the rest of the panel below at
    // most three lines of text, so it stays large enough to use.
    const bool image = row.mine && row.hasImage && env.drawCardImage;
    int maxLines = (panelBottom - y - 8) / advance;
    if (maxLines < 1) return;
    if (maxLines > (portrait ? 7 : 8)) maxLines = portrait ? 7 : 8;
    if (image && maxLines > 3) maxLines = 3;
    y += drawWrappedFixed(renderer, bodyFont, x + 12, y, row.activity, cw - 24, maxLines, advance) * advance;
    if (image && panelBottom - 10 - (y + 8) >= 48)
      env.drawCardImage(env.context, renderer, row.cardId, x + 12, y + 8, cw - 24, panelBottom - 10 - (y + 8));
  };

  auto drawUtilities = [&](int x, int y, int cw, int ch) {
    const int panelBottom = y + ch;
    renderer.drawRect(x, y, cw, ch, 2, true);
    const int inset = 12;
    x += inset;
    y += 10;
    cw -= inset * 2;
    const bool hasEvent = view.profile.nextEvent && renderGlanceSnapshot.eventCount > 0;
    const int eventH = hasEvent ? line10 + line12 + 18 : 0;
    const int weatherBottom = panelBottom - 9 - eventH;
    if (renderGlanceSnapshot.weather.valid) {
      const char* place = renderGlanceSnapshot.weather.place[0] ? renderGlanceSnapshot.weather.place : s.weather;
      const int placeFont = fontForText(SMALL_FONT_ID, place);
      renderer.drawText(placeFont, x, y, renderer.truncatedText(placeFont, place, cw * 2 / 3).c_str(), true,
                        EpdFontFamily::BOLD);
      char weatherStamp[20] = {0};
      if (formatWeatherSnapshotDate(weatherStamp, sizeof(weatherStamp), renderGlanceSnapshot.weather)) {
        if (view.snapshotStale) strncat(weatherStamp, " SAVED", sizeof(weatherStamp) - strlen(weatherStamp) - 1);
        const int stampW = renderer.getTextWidth(SMALL_FONT_ID, weatherStamp, EpdFontFamily::BOLD);
        renderer.drawText(SMALL_FONT_ID, x + cw - stampW, y, weatherStamp, true, EpdFontFamily::BOLD);
      }

      // Hero across the full width, grid beneath. The wrapped rain sentence and
      // its divider are gone: they stole a third of the row to say what the
      // grid's inverted chip now says at a glance.
      const int nowY = y + renderer.getLineHeight(placeFont) + 6;
      const int gridRoom = weatherBottom - nowY - 58;
      const int gridH = renderGlanceSnapshot.weather.dayCount >= 2 && gridRoom >= 74 ? std::min(116, gridRoom) : 0;
      const int gridY = weatherBottom - gridH;
      drawWeatherPoster(renderer, renderGlanceSnapshot.weather, x, nowY, cw, std::max(52, gridY - nowY - 6));
      if (gridH > 0) drawForecastGrid(renderer, renderGlanceSnapshot.weather, x, gridY, cw, gridH);
    } else {
      drawWeatherPlaceholder(renderer, x, y, cw, weatherBottom - y, s.noWeather, s.weatherHint, env.text);
    }

    if (hasEvent) {
      char line[96];
      const int eventTop = panelBottom - eventH;
      renderer.drawLine(x, eventTop, x + cw, eventTop);
      int ey = sectionHeader(x, eventTop + 5, cw, s.nextEvent, nullptr);
      if (GlanceFormat::formatEventLine(line, sizeof(line), renderGlanceSnapshot.events[0]) > 0) {
        const int eventFont = fontForText(UI_10_FONT_ID, line);
        renderer.drawText(eventFont, x, ey, renderer.truncatedText(eventFont, line, cw, EpdFontFamily::BOLD).c_str(),
                          true, EpdFontFamily::BOLD);
      }
    }
  };

  const int primaryGap = 8;
  const bool eventBand =
      readingPage && view.profile.nextEvent && view.profile.weather == DailyProfile::WeatherPanel::Off;
  const int eventHeight = eventBand ? line12 + 22 : 0;
  const int availableH = hintTop - contentTop - eventHeight;
  int primaryX = pad;
  int primaryY = contentTop;
  int primaryW = w - pad * 2;
  int primaryH = availableH;
  auto drawPrimary = [&](int x, int y, int cw, int ch) {
    if (n > 0 && (rows[selectedIndex].reading || rows[selectedIndex].shelfIndex >= 0)) {
      if (view.profile.shows(DailyProfile::HomeItem::ClockCompact)) {
        char time[8] = "--:--";
        if (view.clock.minute < 1440)
          snprintf(time, sizeof(time), "%02u:%02u", view.clock.minute / 60u, view.clock.minute % 60u);
        const int height = line12 + 12;
        centeredIn(UI_12_FONT_ID, x, cw, y + 4, time, EpdFontFamily::BOLD);
        y += height;
        ch -= height;
      }
    }
    if (n > 0 && rows[selectedIndex].shelfIndex >= 0) {
      if (view.profile.shows(DailyProfile::HomeItem::ReadingList))
        drawRecentBooks(renderer, view, env, x, y, cw, ch, rows[selectedIndex].shelfIndex);
      else
        drawReading(x, y, cw, ch);
    } else if (n > 0 && rows[selectedIndex].calendarPage) {
      drawCalendar(renderer, view, env, x, y, cw, ch);
    } else if (n > 0 && rows[selectedIndex].reading) {
      drawReading(x, y, cw, ch);
    } else {
      drawStudy(x, y, cw, ch);
    }
  };
  // Weather/forecast panel placement from the profile; Off gives its space to
  // the primary item.
  const auto weatherPanel = view.profile.weather;
  if (weatherPanel == PocketDaily::DailyProfile::WeatherPanel::Off || (n > 0 && rows[selectedIndex].calendarPage)) {
    drawPrimary(primaryX, primaryY, primaryW, primaryH);
  } else if (portrait) {
    // Recovered chrome space belongs to the hero. This still fits the full
    // five-day graph, then returns the remainder to cover and progress.
    const int utilityH = std::max(250, std::min(272, availableH * 40 / 100));
    primaryH = availableH - utilityH - primaryGap;
    if (weatherPanel == PocketDaily::DailyProfile::WeatherPanel::Top) {
      drawUtilities(pad, primaryY, w - pad * 2, utilityH);
      drawPrimary(primaryX, primaryY + utilityH + primaryGap, primaryW, primaryH);
    } else {
      drawPrimary(primaryX, primaryY, primaryW, primaryH);
      drawUtilities(pad, primaryY + primaryH + primaryGap, w - pad * 2, utilityH);
    }
  } else {
    const int colGap = 10;
    primaryW = (w - pad * 2 - colGap) * 54 / 100;
    drawPrimary(primaryX, primaryY, primaryW, primaryH);
    drawUtilities(primaryX + primaryW + colGap, primaryY, w - pad - (primaryX + primaryW + colGap), primaryH);
  }

  if (eventBand) {
    char event[112]{};
    if (view.glance->eventCount)
      GlanceFormat::formatEventLine(event, sizeof(event), view.glance->events[0]);
    else
      snprintf(event, sizeof(event), "%s", s.noEvents);
    const int y = hintTop - eventHeight + 6;
    renderer.drawLine(pad, y - 6, w - pad, y - 6);
    const int font = env.text.resolve(event, UI_12_FONT_ID);
    renderer.drawText(font, pad, y, renderer.truncatedText(font, event, w - 2 * pad).c_str(), true);
  }
  if (sidePaging) HomeDraw::drawPocketSideChevrons(renderer, view.isX3);

  // Confirm selects the current carousel item. Left opens the Articles library;
  // Right opens Pocket's account-free nearby transport, where the companion
  // sends cards and the glance.
  HomeDraw::drawPocketActionStrip(renderer, view.isX3, env.labels, s.home,
                                  readingPage && !renderReadingSnapshot.valid ? s.library : s.select, s.views, s.sync);
}

void renderBrief(GfxRenderer& renderer, const BriefView& view, const Env& env) {
  const bool isSleep = view.isSleep;
  const auto& m = env.metrics;
  const auto& s = view.strings;
  const int w = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();
  const int pad = m.contentSidePadding;
  const int line12 = renderer.getLineHeight(UI_12_FONT_ID);
  const int line10 = renderer.getLineHeight(UI_10_FONT_ID);
  const int lineS = renderer.getLineHeight(SMALL_FONT_ID);
  // Local names match the activity members this body was moved from.
  const PocketDaily::Glance& g = *view.glance;
  const Reading& renderReadingSnapshot = view.reading;
  const PocketDaily::Card& renderPocketSnapshot = *view.pocketCard;
  auto fontForText = [&](int uiFontId, const char* text) { return env.text.resolve(text, uiFontId); };
  auto drawReadingCover = [&](int x, int y, int width, int height) {
    if (env.drawCover && env.drawCover(env.context, renderer, x, y, width, height)) return true;
    drawTitleCover(renderer, env, renderReadingSnapshot, x, y, width, height);
    return false;
  };
  using HomeDraw::drawForecastGrid;
  using HomeDraw::drawWeatherPoster;
  using HomeDraw::drawWrappedFixed;
  using HomeDraw::formatWeatherSnapshotDate;

  renderer.clearScreen();
  // A reading-only sleep layout uses the whole panel for the cover. Keep the
  // ordinary Brief when no artwork is available or more sections are selected.
  using PocketDaily::DailyProfile::SleepSection;
  const auto& profile = view.profile;
  if (isSleep && view.fullScreenCover && view.sleepCover && renderReadingSnapshot.valid && profile.sleepCount == 1 &&
      profile.sleeps(SleepSection::Reading) && env.drawCover && env.drawCover(env.context, renderer, 0, 0, w, pageH)) {
    return;
  }
  if (env.drawHeader)
    env.drawHeader(env.context, renderer, 0, m.topPadding, w, m.headerHeight, s.pocketDaily, view.headerMeta);

  char buf[96];

  // ── Layout ──
  // The face is a set of independent sections that simply drop out when their
  // data is absent (no calendar → no TODAY, no weather → no forecast, …).
  // Landscape panels (X4, 800×480) split into two columns — left: reading and
  // cards, right: weather and TODAY. Portrait (X3) keeps a single column in the
  // profile's order. Every section leads with a small labeled overline rule.
  const int topY = std::max(m.topPadding + m.headerHeight + m.verticalSpacing, view.contentTopInset);
  const int statusY = pageH - lineS - 12;

  // "LABEL ────" overline. CJK-capable: the weather section uses the
  // user-supplied place name as its label. Returns the content start y.
  auto sectionHeader = [&](int x, int y, int cw, const char* label) -> int {
    const int f = fontForText(SMALL_FONT_ID, label);
    renderer.drawText(f, x, y, label, true, EpdFontFamily::BOLD);
    const int lw = renderer.getTextWidth(f, label, EpdFontFamily::BOLD);
    const int labelHeight = renderer.getLineHeight(f);
    const int ly = y + labelHeight / 2 + 2;
    if (lw + 10 < cw) renderer.drawLine(x + lw + 10, ly, x + cw, ly);
    return y + labelHeight + 6;
  };

  // ── READING (local plane: the open book). Device-owned data — valid with
  // no daemon, no network, and no cached deck, which is what makes the glance
  // meaningful on a fully offline device. ──
  auto drawReading = [&](int x, int y, int cw, int maxY) -> int {
    if (y + line12 * 2 + 12 >= maxY) return y;
    y = sectionHeader(x, y, cw, s.continueReading);
    if (!renderReadingSnapshot.valid) {
      const int titleFont = fontForText(UI_10_FONT_ID, s.noOpenBook);
      y += drawWrappedFixed(renderer, titleFont, x, y + 12, s.noOpenBook, cw, 2, renderer.getLineHeight(titleFont) + 3,
                            EpdFontFamily::BOLD) *
               (renderer.getLineHeight(titleFont) + 3) +
           24;
      const int hintFont = fontForText(SMALL_FONT_ID, s.startReading);
      y += drawWrappedFixed(renderer, hintFont, x, y, s.startReading, cw, 2, renderer.getLineHeight(hintFont) + 3) *
           (renderer.getLineHeight(hintFont) + 3);
      return y + 18;
    }

    // The retained face gives the current book a real visual identity. The
    // setting is intentionally cover-only: turning it off keeps resume/title
    // information useful while avoiding artwork on a desk or bedside panel.
    // A profile may place reading below other sections: the cover shrinks to
    // the room left, and a panel too small for a readable cover uses the
    // compact text form below instead of overlapping the status line.
    const bool portraitFace = pageH > w;
    int coverW =
        portraitFace ? std::min(286, std::max(184, cw * 56 / 100)) : std::min(160, std::max(108, cw * 43 / 100));
    if (y + coverW * 3 / 2 + 17 > maxY) coverW = std::max(0, (maxY - y - 17) * 2 / 3);
    if (isSleep && view.sleepCover && coverW >= 108) {
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
    renderer.drawText(tf, x, y,
                      renderer.truncatedText(tf, renderReadingSnapshot.title, titleW, EpdFontFamily::BOLD).c_str(),
                      true, EpdFontFamily::BOLD);
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
    if (!renderPocketSnapshot.cardId[0] || y + line12 * 2 + 12 >= maxY) return y;
    y = sectionHeader(x, y, cw, s.study);
    const int titleFont = fontForText(UI_12_FONT_ID, renderPocketSnapshot.title);
    renderer.drawText(
        titleFont, x, y,
        renderer
            .truncatedText(titleFont, renderPocketSnapshot.title[0] ? renderPocketSnapshot.title : s.study, cw,
                           EpdFontFamily::BOLD)
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

  // The first of the companion's cards, shown whether or not a book is open
  // (for example "if found, please contact"), with its image when it fits.
  auto drawPinned = [&](int x, int y, int cw, int maxY) -> int {
    const PocketDaily::Card* card = view.pinnedCard;
    if (!card || !card->cardId[0] || y + line12 * 2 + 12 >= maxY) return y;
    y = sectionHeader(x, y, cw, s.myCards);
    const int titleFont = fontForText(UI_12_FONT_ID, card->title);
    renderer.drawText(titleFont, x, y, renderer.truncatedText(titleFont, card->title, cw, EpdFontFamily::BOLD).c_str(),
                      true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(titleFont) + 5;
    const int bodyFont = fontForText(UI_10_FONT_ID, card->question);
    const int advance = renderer.getLineHeight(bodyFont) + 2;
    int maxLines = (maxY - y) / advance;
    if (maxLines > 3) maxLines = 3;
    if (maxLines > 0) y += drawWrappedFixed(renderer, bodyFont, x, y, card->question, cw, maxLines, advance) * advance;
    if (view.pinnedHasImage && env.drawCardImage) {
      const int imageH = std::min(maxY - y - 6, 300);
      const int drawn = imageH >= 48 ? env.drawCardImage(env.context, renderer, card->cardId, x, y + 6, cw, imageH) : 0;
      if (drawn > 0) y += 6 + drawn;
    }
    return y + 12;
  };

  // ── Weather (the walking-out-the-door read; label = place name) ──
  auto drawWeather = [&](int x, int y, int cw, int maxY) -> int {
    // A section that cannot fit its header and the temperature poster is
    // skipped rather than drawn over the status line.
    if (y + line12 + 58 >= maxY) return y;
    if (!g.weather.valid) {
      y = sectionHeader(x, y, cw, s.weather);
      const int height = std::min(160, maxY - y);
      HomeDraw::drawWeatherPlaceholder(renderer, x, y, cw, height, s.noWeather, s.weatherHint, env.text);
      return y + height;
    }
    char weatherLabel[BRIEF_WEATHER_LABEL_BYTES] = {0};
    briefWeatherLabel(weatherLabel, sizeof(weatherLabel), g.weather, s.weather, view.snapshotStale);
    y = sectionHeader(x, y, cw, weatherLabel);
    // The retained frame uses the same poster + grid so a woken device and a
    // powered-off one do not look like two different products.
    const int gridRoom = maxY - y - 58;
    const int gridH = g.weather.dayCount >= 2 && gridRoom >= 74 ? std::min(116, gridRoom) : 0;
    const int gridY = maxY - gridH;
    if (y + line12 < maxY) drawWeatherPoster(renderer, g.weather, x, y, cw, std::max(52, gridY - y - 6));
    if (gridH > 0 && gridY >= y + 48) {
      const int gridDrawn = drawForecastGrid(renderer, g.weather, x, gridY, cw, gridH);
      if (gridDrawn > 0) return std::min(maxY, gridY + gridDrawn + 10);
    }
    return std::min(maxY, y + 74);
  };

  // ── TODAY (daemon-authored schedule, absolute HH:MM only). Absent when no
  // calendar is configured — the layout simply flows past it. ──
  auto drawToday = [&](int x, int y, int cw, int maxY) -> int {
    // No room for the header plus one event: skip rather than overlap the
    // status line (sections above may have used the space).
    const bool onlyToday = view.profile.sleepCount == 1 && view.profile.sleeps(DailyProfile::SleepSection::Today);
    if ((g.eventCount == 0 && !onlyToday) || y + lineS + 6 + line10 >= maxY) return y;
    y = sectionHeader(x, y, cw, s.today);
    if (g.eventCount == 0) {
      const int f = fontForText(UI_10_FONT_ID, s.noEvents);
      const int height = renderer.getLineHeight(f);
      if (y + height >= maxY) return y;
      renderer.drawText(f, x, y, renderer.truncatedText(f, s.noEvents, cw).c_str(), true);
      return y + height + 12;
    }
    for (uint8_t i = 0; i < g.eventCount; i++) {
      if (y + line10 >= maxY) break;
      if (GlanceFormat::formatEventLine(buf, sizeof(buf), g.events[i]) <= 0) continue;
      const int f = fontForText(UI_10_FONT_ID, buf);
      renderer.drawText(f, x, y, renderer.truncatedText(f, buf, cw).c_str(), true);
      y += line10 + 4;
    }
    return y + 12;
  };

  // Pocket Glance is deliberately personal and locally meaningful: current
  // book, one carried study item, weather and today's schedule.
  // Sleep sections and their order come from the profile (defaults: reading,
  // study, weather, today). Study stays a fallback while a book is shown.
  const bool studyShown = !renderReadingSnapshot.valid || !profile.sleeps(SleepSection::Reading);
  if (isSleep && pageH > w) {
    // The retained portrait is a glance, not a dashboard; sections that no
    // longer fit are skipped by each drawer's own bounds.
    int y = topY;
    for (uint8_t k = 0; k < profile.sleepCount; ++k) {
      switch (profile.sleepSections[k]) {
        case SleepSection::Reading:
          y = drawReading(pad, y, w - pad * 2, statusY - 8);
          break;
        case SleepSection::Study:
          if (studyShown) y = drawStudy(pad, y, w - pad * 2, statusY - 8);
          break;
        case SleepSection::Weather: {
          // Weather fills to its bottom bound; when sections follow, give it the
          // same fixed panel height Home uses so they keep their room.
          const bool last = k + 1 == profile.sleepCount;
          const int bound = last ? statusY - 8 : std::min(statusY - 8, y + 272);
          y = drawWeather(pad, y, w - pad * 2, bound);
          break;
        }
        case SleepSection::Today:
          y = drawToday(pad, y, w - pad * 2, statusY - 8);
          break;
        case SleepSection::Card:
          y = drawPinned(pad, y, w - pad * 2, statusY - 8);
          break;
      }
    }
  } else if (isSleep) {
    // Wide retained panels keep two calm columns; visibility follows the profile.
    const int gap = 20;
    const int colW = (w - pad * 2 - gap) / 2;
    int leftY = profile.sleeps(SleepSection::Reading) ? drawReading(pad, topY, colW, statusY - 8) : topY;
    if (profile.sleeps(SleepSection::Study) && studyShown) leftY = drawStudy(pad, leftY, colW, statusY - 8);
    if (profile.sleeps(SleepSection::Card)) drawPinned(pad, leftY, colW, statusY - 8);
    int rightY = profile.sleeps(SleepSection::Weather) ? drawWeather(pad + colW + gap, topY, colW, statusY - 8) : topY;
    if (profile.sleeps(SleepSection::Today)) drawToday(pad + colW + gap, rightY, colW, statusY - 8);
  } else if (pageH > w) {
    int y = topY;
    y = drawReading(pad, y, w - pad * 2, statusY - 8);
    y = drawStudy(pad, y, w - pad * 2, statusY - 8);
    y = drawWeather(pad, y, w - pad * 2, statusY - 8);
    drawToday(pad, y, w - pad * 2, statusY - 8);
  } else {
    const int gap = 20;
    const int colW = (w - pad * 2 - gap) / 2;
    int leftY = drawReading(pad, topY, colW, statusY - 8);
    drawStudy(pad, leftY, colW, statusY - 8);
    int rightY = drawWeather(pad + colW + gap, topY, colW, statusY - 8);
    drawToday(pad + colW + gap, rightY, colW, statusY - 8);
  }

  // Localized status text (Korean, for example) needs the CJK font like every
  // other label on this face; Latin text keeps the built-in small font.
  const int statusFont = fontForText(SMALL_FONT_ID, view.status);
  renderer.drawText(statusFont, pad, statusY,
                    renderer.truncatedText(statusFont, view.status, w - pad * 2, EpdFontFamily::BOLD).c_str(), true,
                    EpdFontFamily::BOLD);
}
}  // namespace PocketDaily::Home
