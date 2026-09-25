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
        if (available.book) out[n++] = RowSource::Reading;
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
      case HomeItem::Provider:
      case HomeItem::Monitor:
        // Retired with the AgentDeck daemon: still valid in stored profiles,
        // never shown (docs/pocket-profile-v1.md).
        break;
    }
  }
  return n;
}

void renderHome(GfxRenderer& renderer, const HomeView& view, const Env& env) {
  const auto& m = env.metrics;
  const auto& s = view.strings;
  const OverviewRow* rows = view.rows;
  const int n = view.count;
  const int w = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();
  const bool sidePaging = n > 1;
  // X3 gets borderless edge chevrons instead of the old 30px PREV/NEXT rails.
  // X4 has enough width to retain its hardware-aligned side hints.
  const int pad =
      sidePaging && !view.isX3 ? std::max(m.contentSidePadding, m.sideButtonHintsWidth + 6) : m.contentSidePadding;
  const int line12 = renderer.getLineHeight(UI_12_FONT_ID);
  const int line10 = renderer.getLineHeight(UI_10_FONT_ID);
  // Local names match the activity members this body was moved from.
  const PocketDaily::Glance& renderGlanceSnapshot = *view.glance;
  const Reading& renderReadingSnapshot = view.reading;
  auto fontForText = [&](int uiFontId, const char* text) { return env.text.resolve(text, uiFontId); };
  auto drawReadingCover = [&](int x, int y, int width, int height) {
    return env.drawCover && env.drawCover(env.context, renderer, x, y, width, height);
  };
  using HomeDraw::drawForecastGrid;
  using HomeDraw::drawWeatherPlaceholder;
  using HomeDraw::drawWeatherPoster;
  using HomeDraw::drawWrappedFixed;
  using HomeDraw::formatWeatherSnapshotDate;

  renderer.clearScreen();
  if (env.drawHeader) env.drawHeader(env.context, renderer, 0, m.topPadding, w, m.headerHeight, s.pocketDaily, nullptr);
  // Same top as the sleep face so Home and the retained frame line up.
  const int contentTop = m.topPadding + m.headerHeight + m.verticalSpacing;
  const int hintTop = pageH - renderer.getLineHeight(SMALL_FONT_ID) - 16;
  const bool portrait = pageH > w;

  // The row list is also the visible carousel: the current book and carried
  // study items use the same large content area instead of competing cards.
  const int selectedIndex = view.selected >= 0 && view.selected < n ? view.selected : 0;
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
      y = sectionHeader(x + 12, y + 10, cw - 24, s.continueReading, carouselPosition);
      renderer.drawCenteredText(UI_10_FONT_ID, y + (panelBottom - y) / 2 - line10, s.noOpenBook, true,
                                EpdFontFamily::BOLD);
      renderer.drawCenteredText(SMALL_FONT_ID, y + (panelBottom - y) / 2 + 6, s.startReading, true);
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
    const char* readingLabel = s.continueReading;
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
      drawWeatherPlaceholder(renderer, x, y, cw, weatherBottom - y, s.noWeather, s.weatherHint);
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
  const int availableH = hintTop - contentTop;
  int primaryX = pad;
  int primaryY = contentTop;
  int primaryW = w - pad * 2;
  int primaryH = availableH;
  auto drawPrimary = [&](int x, int y, int cw, int ch) {
    if (n > 0 && rows[selectedIndex].reading)
      drawReading(x, y, cw, ch);
    else
      drawStudy(x, y, cw, ch);
  };
  // Weather/forecast panel placement from the profile; Off gives its space to
  // the primary item.
  const auto weatherPanel = view.profile.weather;
  if (weatherPanel == PocketDaily::DailyProfile::WeatherPanel::Off) {
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

  if (sidePaging) HomeDraw::drawPocketSideChevrons(renderer, view.isX3);

  // Confirm selects the current carousel item. Right opens Pocket's account-free
  // nearby transport, where the companion sends cards and the glance.
  HomeDraw::drawPocketActionStrip(renderer, view.isX3, env.labels, s.library, s.select, s.sync);
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
    return env.drawCover && env.drawCover(env.context, renderer, x, y, width, height);
  };
  using HomeDraw::drawForecastGrid;
  using HomeDraw::drawWeatherPoster;
  using HomeDraw::drawWrappedFixed;
  using HomeDraw::formatWeatherSnapshotDate;

  renderer.clearScreen();
  if (env.drawHeader)
    env.drawHeader(env.context, renderer, 0, m.topPadding, w, m.headerHeight, s.pocketDaily, view.headerMeta);

  char buf[96];

  // ── Layout ──
  // The face is a set of independent sections that simply drop out when their
  // data is absent (no calendar → no TODAY, no weather → no forecast, …).
  // Landscape panels (X4, 800×480) split into two columns — left: reading and
  // cards, right: weather and TODAY. Portrait (X3) keeps a single column in the
  // profile's order. Every section leads with a small labeled overline rule.
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
  auto drawReading = [&](int x, int y, int cw, int maxY) -> int {
    if (!renderReadingSnapshot.valid || y + line12 * 2 + 12 >= maxY) return y;
    y = sectionHeader(x, y, cw, s.continueReading);

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
    if (!g.weather.valid || y + line12 + 58 >= maxY) return y;
    char weatherLabel[56] = {0};
    snprintf(weatherLabel, sizeof(weatherLabel), "%s", g.weather.place[0] ? g.weather.place : s.weather);
    char snapshotDate[8] = {0};
    if (formatWeatherSnapshotDate(snapshotDate, sizeof(snapshotDate), g.weather))
      snprintf(weatherLabel + strlen(weatherLabel), sizeof(weatherLabel) - strlen(weatherLabel), " \xC2\xB7 %s%s",
               snapshotDate, view.snapshotStale ? " SAVED" : "");
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
    if (g.eventCount == 0 || y + lineS + 6 + line10 >= maxY) return y;
    y = sectionHeader(x, y, cw, s.today);
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
  using PocketDaily::DailyProfile::SleepSection;
  const auto& profile = view.profile;
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

  renderer.drawText(SMALL_FONT_ID, pad, statusY,
                    renderer.truncatedText(SMALL_FONT_ID, view.status, w - pad * 2, EpdFontFamily::BOLD).c_str(), true,
                    EpdFontFamily::BOLD);
}
}  // namespace PocketDaily::Home
