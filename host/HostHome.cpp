#include "HostHome.h"

#include <GfxRenderer.h>
#include <builtinFonts/notosans_8_regular.h>
#include <builtinFonts/ubuntu_10_bold.h>
#include <builtinFonts/ubuntu_10_regular.h>
#include <builtinFonts/ubuntu_12_bold.h>
#include <builtinFonts/ubuntu_12_regular.h>

#include <cstring>

#include "fontIds.h"

// Host preview inputs for the shared Home / Daily Brief painters. Sample data
// is representative, not a reader's content; strings stay the English defaults.
namespace PocketUIHost {
namespace {
EpdFont smallFont(&notosans_8_regular);
EpdFontFamily smallFamily(&smallFont);
EpdFont ui10Regular(&ubuntu_10_regular);
EpdFont ui10Bold(&ubuntu_10_bold);
EpdFontFamily ui10Family(&ui10Regular, &ui10Bold);
EpdFont ui12Regular(&ubuntu_12_regular);
EpdFont ui12Bold(&ubuntu_12_bold);
EpdFontFamily ui12Family(&ui12Regular, &ui12Bold);

void copy(char* out, size_t cap, const char* text) {
  strncpy(out, text, cap - 1);
  out[cap - 1] = '\0';
}

// Built-in UI fonts cover Latin (incl. Latin-1 punctuation such as the middle
// dot); Hangul, Kana and CJK need the bundled cpfont, as UiCjkFont decides on
// the device.
bool needsCjkFont(const char* text) {
  const auto* p = reinterpret_cast<const unsigned char*>(text ? text : "");
  while (*p) {
    uint32_t cp = *p;
    int extra = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
    if (extra) cp &= 0x3F >> extra;
    ++p;
    for (; extra > 0 && *p; --extra, ++p) cp = (cp << 6) | (*p & 0x3F);
    if ((cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x4E00 && cp <= 0x9FFF) ||
        (cp >= 0xAC00 && cp <= 0xD7A3))
      return true;
  }
  return false;
}
}  // namespace

void registerUiFonts(GfxRenderer& renderer) {
  renderer.insertFont(UI_10_FONT_ID, ui10Family);
  renderer.insertFont(UI_12_FONT_ID, ui12Family);
  renderer.insertFont(SMALL_FONT_ID, smallFamily);
}

PocketDaily::Home::Env hostEnv(GfxRenderer& renderer) {
  PocketDaily::Home::Env env;
  // Base theme values (BaseTheme.h); the device reports its own via display.
  env.metrics = {5, 45, 10, 20, 30};
  // Built-in fonts cover Latin; anything else uses the context's cpfont (id 1).
  const PocketDaily::HomeDraw::FontResolver fonts{
      &renderer, [](void* context, const char* text, int fallback, EpdFontFamily::Style) {
        if (!needsCjkFont(text)) return fallback;
        auto& r = *static_cast<GfxRenderer*>(context);
        return r.ensureSdCardFontReady(1, text, 3) == 0 ? 1 : fallback;
      }};
  env.text = fonts;
  env.labels = fonts;
  env.drawHeader = [](void*, const GfxRenderer& r, int x, int y, int width, int height, const char* title,
                      const char* subtitle) {
    const int line = r.getLineHeight(UI_12_FONT_ID);
    const int baseline = y + (height - line) / 2;
    r.drawText(UI_12_FONT_ID, x + 20, baseline, title, true, EpdFontFamily::BOLD);
    if (subtitle && subtitle[0]) {
      const int sw = r.getTextWidth(SMALL_FONT_ID, subtitle);
      r.drawText(SMALL_FONT_ID, x + width - 20 - sw, baseline + 4, subtitle, true);
    }
    r.drawLine(x + 20, y + height - 2, x + width - 20, y + height - 2, 2, true);
  };
  env.drawCover = [](void*, GfxRenderer& r, int x, int y, int width, int height) {
    // A hatched stand-in: the preview has no book file to decode.
    r.drawRect(x + 2, y + 2, width - 4, height - 4, 2, true);
    for (int d = 0; d < width + height; d += 12) {
      const int x0 = x + 2 + std::max(0, d - (height - 4));
      const int y0 = y + 2 + std::min(d, height - 4);
      const int x1 = x + 2 + std::min(d, width - 4);
      const int y1 = y + 2 + std::max(0, d - (width - 4));
      r.drawLine(x0, y0, x1, y1, 1, true);
    }
    return true;
  };
  return env;
}

Sample buildSample(uint32_t samples) {
  Sample s;
  s.glance.clear();
  s.card = {};
  if (samples & SampleWeather) {
    auto& w = s.glance.weather;
    w.valid = true;
    copy(w.place, sizeof(w.place), "Seoul");
    w.code = 2;
    w.tempC = 18;
    copy(w.summary, sizeof(w.summary), "Cloudy");
    w.todayMinC = 12;
    w.todayMaxC = 21;
    copy(w.rainStartHm, sizeof(w.rainStartHm), "14:00");
    copy(w.rainEndHm, sizeof(w.rainEndHm), "17:00");
    w.rainProbability = 60;
    const struct {
      const char* date;
      int16_t code;
      int8_t min, max, rain;
    } days[] = {{"2026-09-25", 2, 12, 21, 60},
                {"2026-09-26", 0, 11, 23, 10},
                {"2026-09-27", 61, 13, 19, 80},
                {"2026-09-28", 1, 10, 22, 20},
                {"2026-09-29", 0, 9, 24, 0}};
    w.dayCount = 5;
    for (int i = 0; i < 5; ++i) {
      copy(w.days[i].date, sizeof(w.days[i].date), days[i].date);
      w.days[i].code = days[i].code;
      w.days[i].minC = days[i].min;
      w.days[i].maxC = days[i].max;
      w.days[i].rainProbability = days[i].rain;
    }
    w.tomorrow = w.days[1];
    s.glance.valid = true;
  }
  if (samples & SampleEvents) {
    const char* events[][3] = {{"09:30", "10:00", "Team standup"}, {"13:00", "14:00", "Lunch with Min"}};
    for (auto& e : events) {
      auto& out = s.glance.events[s.glance.eventCount++];
      copy(out.startHm, sizeof(out.startHm), e[0]);
      copy(out.endHm, sizeof(out.endHm), e[1]);
      copy(out.title, sizeof(out.title), e[2]);
    }
    s.glance.valid = true;
  }
  if (samples & SampleUsage) {
    auto& a = s.glance.usage[s.glance.usageCount++];
    copy(a.provider, sizeof(a.provider), "claude");
    copy(a.label, sizeof(a.label), "5h window");
    a.primaryPercent = 42;
    copy(a.primaryResetHm, sizeof(a.primaryResetHm), "16:00");
    auto& b = s.glance.usage[s.glance.usageCount++];
    copy(b.provider, sizeof(b.provider), "codex");
    copy(b.label, sizeof(b.label), "weekly");
    b.primaryPercent = 71;
    b.secondaryPercent = -1;
    copy(s.glance.wrapup[s.glance.wrapupCount++], PocketDaily::Glance::WRAPUP_BYTES, "Finished the sync fix.");
    s.glance.valid = true;
  }
  if (samples & SampleStudy) {
    copy(s.card.cardId, sizeof(s.card.cardId), "sample:study");
    copy(s.card.module, sizeof(s.card.module), "app");
    copy(s.card.title, sizeof(s.card.title), "Word of the day");
    copy(s.card.question, sizeof(s.card.question), "serendipity - finding something good without looking for it");
  }
  s.reading = {(samples & SampleBook) != 0, "Pride and Prejudice", "Jane Austen", 42};
  return s;
}

// Mirrors PocketDailyActivity::collectOverview's profile loop over sources.
int buildRows(const PocketDaily::DailyProfile::Profile& profile, const Sample& sample, uint32_t samples,
              PocketDaily::Home::Row* rows, int cap) {
  using PocketDaily::Home::RowSource;
  PocketDaily::Home::RowAvailability available;
  available.book = samples & SampleBook;
  available.appCards = samples & SampleStudy;
  available.provider = samples & SampleProvider;
  available.monitor = samples & SampleUsage;
  RowSource sources[PocketDaily::DailyProfile::HOME_ITEM_CAP];
  const int count = PocketDaily::Home::homeRowSources(profile, available, sources);
  int n = 0;
  for (int k = 0; k < count && n < cap; ++k) {
    switch (sources[k]) {
      case RowSource::Reading:
        rows[n++] = {true, false, false, "Continue Reading", "Pride and Prejudice"};
        break;
      case RowSource::AppCards:
        rows[n++] = {false, true, false, sample.card.title, sample.card.question};
        break;
      case RowSource::DailyWord:
        rows[n++] = {false, true, false, "Daily word", "An offline word from the reader"};
        break;
      case RowSource::Provider:
        rows[n++] = {false, true, false, "Pocket item", "A carried provider card"};
        break;
      case RowSource::Monitor:
        rows[n++] = {false, false, true, "Monitoring", ""};
        break;
    }
  }
  return n;
}
}  // namespace PocketUIHost
