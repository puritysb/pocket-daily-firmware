#include "ScreenFrame.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalSystem.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <initializer_list>

#include "CrossPointSettings.h"
#include "SdCardFontSystem.h"
#include "components/UITheme.h"
#include "pocket_daily/PocketGlanceStore.h"
#include "pocket_daily/home/DailyWord.h"
#include "pocket_daily/home/GlanceFormat.h"
#include "util/CjkScript.h"

namespace PocketDaily::Screen {
namespace {
// Never drawn: stands in for the daily word when another row or section is
// the one on screen, so row counts match the reader's own Home.
constexpr char kWordPlaceholderId[] = "local:word";

const PocketDaily::Card* firstAppCard(const Frame& frame) {
  const auto* cards = frame.cards.cards();
  return cards && cards->count ? &cards->cards[0].card : nullptr;
}

// Same rule as renderBrief: study is a fallback while a book is shown.
bool briefStudyShown(const Frame& frame) {
  return frame.profile.sleeps(DailyProfile::SleepSection::Study) &&
         (!frame.book.valid || !frame.profile.sleeps(DailyProfile::SleepSection::Reading));
}

// The Brief's carried study card: the first companion card, else the word.
const PocketDaily::Card& briefStudyCard(const Frame& frame) {
  const auto* first = firstAppCard(frame);
  return first ? *first : frame.word;
}

// Mirrors PocketDailyActivity::collectOverview over the shared
// homeRowSources. Returns true when the first row is the daily word.
bool buildHomeRows(Frame& frame) {
  const auto* appCards = frame.cards.cards();
  Home::RowAvailability available;
  available.book = frame.book.valid;
  available.appCards = appCards && appCards->count;
  Home::RowSource sources[DailyProfile::HOME_ITEM_CAP];
  const int count = Home::homeRowSources(frame.profile, available, sources);
  int n = 0;
  bool firstIsWord = false;
  auto append = [&](const Home::Row& row) {
    if (n < Home::ROW_CAP) frame.rows[n++] = row;
  };
  for (int k = 0; k < count && n < Home::ROW_CAP; ++k) {
    switch (sources[k]) {
      case Home::RowSource::Reading:
        // renderHome draws the reading row from HomeView::reading.
        append({true, false, false, false, false, "local:reading", "", ""});
        break;
      case Home::RowSource::AppCards:
        for (uint8_t i = 0; appCards && i < appCards->count && n < Home::ROW_CAP; ++i) {
          const auto& card = appCards->cards[i];
          if (!card.card.cardId[0]) continue;
          append({false, true, true, false, card.imagePath[0] != '\0', card.card.cardId, "", ""});
        }
        break;
      case Home::RowSource::DailyWord:
        if (n == 0) firstIsWord = true;
        append({false, true, false, true, false, kWordPlaceholderId, "", ""});
        break;
    }
  }
  frame.rowCount = n;
  return firstIsWord;
}

// Composes the first row's text exactly as the reader's Home row.
void composeFirstRow(Frame& frame) {
  if (frame.rowCount == 0 || !frame.rows[0].pocket) return;
  Home::Row& row = frame.rows[0];
  const PocketDaily::Card* card = &frame.word;
  if (row.mine) {
    const auto* appCards = frame.cards.cards();
    for (uint8_t i = 0; appCards && i < appCards->count; ++i)
      if (strcmp(appCards->cards[i].card.cardId, row.cardId) == 0) card = &appCards->cards[i].card;
  } else {
    row.cardId = frame.word.cardId;
  }
  snprintf(frame.firstProject, sizeof(frame.firstProject), "%s", Home::cardRowTitle(*card));
  Home::cardRowActivity(*card, frame.firstActivity, sizeof(frame.firstActivity));
  row.project = frame.firstProject;
  row.activity = frame.firstActivity;
}

struct DrawContext {
  const Frame* frame;
  int font;
  bool refused;
};

// FontResolver::pick is a fixed `void*` callback type shared with the host
// preview; a `const void*` parameter would not convert to it.
// cppcheck-suppress constParameterPointer
int pickFont(void* context, const char* text, const int fallback, EpdFontFamily::Style) {
  const int font = static_cast<const DrawContext*>(context)->font;
  return font && CjkScript::needsCjkFont(text) ? font : fallback;
}

bool visitStrings(void* context, bool (*visit)(void*, const char*), std::initializer_list<const char*> texts) {
  for (const char* text : texts)
    if (text && text[0] && !visit(context, text)) return false;
  return true;
}
}  // namespace

LoadResult loadFrame(const Surface surface, const DailyProfile::Profile& profile, std::unique_ptr<Frame>& out) {
  out.reset();
  if (surface == Surface::None) return LoadResult::Unavailable;
  // ~2.2 KB of paint inputs: too large for the loop-task stack; one fallible
  // allocation per presentation, released after the paint.
  auto frame = makeUniqueNoThrow<Frame>();
  if (!frame) {
    LOG_ERR("SCREEN", "OOM allocating %u B frame inputs", (unsigned)sizeof(Frame));
    return LoadResult::OutOfMemory;
  }
  frame->surface = surface;
  frame->profile = profile;

  // The stored app glance (+404 B record while loading), rolled to today as
  // the Pocket activity does before every paint. Missing = no weather yet.
  if (!AppGlance::load(frame->glance)) frame->glance.clear();
  const time_t now = time(nullptr);
  Home::rollGlanceToToday(frame->glance, now);
  frame->glanceStale = HomeDraw::snapshotIsStale(frame->glance.savedEpoch, now);

  // Active My cards: the same verified snapshot the reader loads on entry.
  switch (frame->cards.load(nullptr, HalSystem::feedWatchdogIfRegistered)) {
    case Content::ContentViewState::LoadResult::Ready:
    case Content::ContentViewState::LoadResult::Empty:
    case Content::ContentViewState::LoadResult::NoActive:
      break;
    case Content::ContentViewState::LoadResult::OutOfMemory:
      return LoadResult::OutOfMemory;
    case Content::ContentViewState::LoadResult::InvalidTarget:
    case Content::ContentViewState::LoadResult::TargetChanged:
    case Content::ContentViewState::LoadResult::Unavailable:
      LOG_ERR("SCREEN", "Active cards unavailable for the screen frame");
      return LoadResult::Unavailable;
  }

  Home::readOpenBook(frame->book);

  // The daily word only when it is the drawn page; otherwise a placeholder.
  bool wordDrawn = false;
  switch (surface) {
    case Surface::Home:
      wordDrawn = buildHomeRows(*frame);
      break;
    case Surface::Brief:
      wordDrawn = !firstAppCard(*frame) && briefStudyShown(*frame);
      break;
    case Surface::None:
      break;
  }
  if (wordDrawn) {
    const uint32_t day =
        Home::dailyWordDay(Home::dailyWordEpoch(now, frame->glance.savedEpoch), frame->glance.utcOffsetMinutes);
    if (Home::lightDailyWord(day, frame->word) == Home::WordSource::OutOfMemory) return LoadResult::OutOfMemory;
  } else {
    snprintf(frame->word.cardId, sizeof(frame->word.cardId), "%s", kWordPlaceholderId);
    snprintf(frame->word.module, sizeof(frame->word.module), "local");
  }
  if (surface == Surface::Home) composeFirstRow(*frame);

  out = std::move(frame);
  return LoadResult::Ready;
}

bool visitFrameText(const Frame& frame, void* context, bool (*visit)(void* context, const char* text)) {
  const Home::Strings s = Home::deviceStrings();
  const auto& g = frame.glance.glance;
  const auto& profile = frame.profile;
  char line[96];
  // Header title (GUI.drawHeader resolves it through UiCjkFont; see drawFrame).
  if (!visitStrings(context, visit, {s.pocketDaily})) return false;
  switch (frame.surface) {
    case Surface::None:
      return false;
    case Surface::Home: {
      if (!visitStrings(context, visit, {s.home, s.select, s.articles, s.sync})) return false;
      const Home::Row* first = frame.rowCount ? &frame.rows[0] : nullptr;
      if (first && first->reading) {
        if (!visitStrings(context, visit, {s.continueReading, frame.book.title, frame.book.author})) return false;
      } else if (first && first->pocket) {
        const char* label = first->mine ? s.myCards : first->word ? s.word : s.study;
        if (!visitStrings(context, visit, {label, first->project, first->activity})) return false;
      } else if (!visitStrings(context, visit, {s.study, s.empty})) {
        return false;
      }
      if (profile.weather == DailyProfile::WeatherPanel::Off) return true;
      if (g.weather.valid && !visitStrings(context, visit, {g.weather.place[0] ? g.weather.place : s.weather}))
        return false;
      if (!g.weather.valid && !visitStrings(context, visit, {s.noWeather, s.weatherHint})) return false;
      if (profile.nextEvent && g.eventCount > 0) {
        if (!visitStrings(context, visit, {s.nextEvent})) return false;
        if (GlanceFormat::formatEventLine(line, sizeof(line), g.events[0]) > 0 && !visitStrings(context, visit, {line}))
          return false;
      }
      return true;
    }
    case Surface::Brief: {
      using DailyProfile::SleepSection;
      if (!visitStrings(context, visit, {tr(STR_POCKET_SLEEP_PREVIEW)})) return false;
      if (profile.sleeps(SleepSection::Reading) && frame.book.valid &&
          !visitStrings(context, visit, {s.continueReading, frame.book.title, frame.book.author}))
        return false;
      const PocketDaily::Card& study = briefStudyCard(frame);
      if (briefStudyShown(frame) && study.cardId[0] &&
          !visitStrings(context, visit, {s.study, study.title[0] ? study.title : s.study, study.question}))
        return false;
      if (profile.sleeps(SleepSection::Weather) && g.weather.valid) {
        char label[Home::BRIEF_WEATHER_LABEL_BYTES];
        Home::briefWeatherLabel(label, sizeof(label), g.weather, s.weather, frame.glanceStale);
        if (!visitStrings(context, visit, {label})) return false;
      }
      if (profile.sleeps(SleepSection::Today) && g.eventCount > 0) {
        if (!visitStrings(context, visit, {s.today})) return false;
        for (uint8_t i = 0; i < g.eventCount; ++i)
          if (GlanceFormat::formatEventLine(line, sizeof(line), g.events[i]) > 0 &&
              !visitStrings(context, visit, {line}))
            return false;
      }
      const auto* pinned = firstAppCard(frame);
      if (profile.sleeps(SleepSection::Card) && pinned &&
          !visitStrings(context, visit, {s.myCards, pinned->title, pinned->question}))
        return false;
      return true;
    }
  }
  return false;
}

bool drawFrame(GfxRenderer& renderer, const Frame& frame, const int cjkFont) {
  DrawContext context{&frame, cjkFont, false};
  Home::Env env;
  env.metrics = Home::deviceMetrics();
  // Latin text keeps the built-in fonts; CJK text uses the one preflighted
  // bounded font. Never UiCjkFont's Cached loads.
  env.text = {&context, pickFont};
  env.labels = env.text;
  env.context = &context;
  env.drawHeader = [](void* self, const GfxRenderer& r, int x, int y, int width, int height, const char* title,
                      const char* subtitle) {
    auto& d = *static_cast<DrawContext*>(self);
    // GUI.drawHeader resolves CJK through UiCjkFont, whose first step reuses
    // the resident font when it covers the text. The presenter checked these
    // strings in regular and bold before clearing; confirm right here that the
    // resident font is still that bounded font and covers them, so the theme
    // can never fall through to a Cached family load. Otherwise refuse.
    for (const char* text : {title, subtitle}) {
      if (!text || !CjkScript::needsCjkFont(text)) continue;
      if (!d.font || sdFontSystem.currentLoadedFontId() != d.font || !sdFontSystem.boundedUiFontReady(d.font) ||
          r.ensureSdCardFontReady(d.font, text, 3) != 0) {
        d.refused = true;
        return;
      }
    }
    GUI.drawHeader(r, Rect{x, y, width, height}, title, subtitle);
  };
  // Covers are never decoded: the device's own no-art placeholder. Env's
  // drawCover type takes a mutable renderer (the device decodes bitmaps).
  // cppcheck-suppress constParameterReference
  env.drawCover = [](void*, GfxRenderer& r, int x, int y, int width, int height) {
    if (width <= 12 || height <= 18) return false;
    r.drawRect(x, y, width, height, 2, true);
    HomeDraw::drawCoverPlaceholder(r, x, y, width, height);
    return false;
  };
  env.drawCardImage = [](void* self, GfxRenderer& r, const char* cardId, int x, int y, int width, int height) {
    auto& d = *static_cast<DrawContext*>(self);
    const int drawn = Home::drawContentCardImage(r, d.frame->cards, cardId, x, y, width, height);
    // The shared painter calls only when a named image fits. A failed SD read
    // must not certify a frame missing that image or change the last panel.
    if (drawn <= 0) d.refused = true;
    return drawn;
  };

  switch (frame.surface) {
    case Surface::None:
      return false;
    case Surface::Home: {
      Home::HomeView view;
      view.isX3 = gpio.deviceIsX3();
      view.rows = frame.rows;
      view.count = frame.rowCount;
      view.selected = 0;  // the reader enters Pocket Daily on the first row
      view.reading = {frame.book.valid, frame.book.title, frame.book.author, frame.book.percent};
      view.glance = &frame.glance.glance;
      view.snapshotStale = frame.glanceStale;
      view.profile = frame.profile;
      view.strings = Home::deviceStrings();
      Home::renderHome(renderer, view, env);
      break;
    }
    case Surface::Brief: {
      Home::BriefView view;
      // A sleep frame without the wake cue; the status line says it is a
      // preview, never that the reader is powered off while its radio is on.
      view.isSleep = true;
      view.headerMeta = nullptr;
      view.glance = &frame.glance.glance;
      view.reading = {frame.book.valid, frame.book.title, frame.book.author, frame.book.percent};
      view.sleepCover = SETTINGS.pocketDailySleepCover;
      view.pocketCard = &briefStudyCard(frame);
      if (const auto* cards = frame.cards.cards(); cards && cards->count) {
        view.pinnedCard = &cards->cards[0].card;
        view.pinnedHasImage = cards->cards[0].imagePath[0] != '\0';
      }
      view.snapshotStale = frame.glanceStale;
      view.status = tr(STR_POCKET_SLEEP_PREVIEW);
      view.profile = frame.profile;
      view.strings = Home::deviceStrings();
      Home::renderBrief(renderer, view, env);
      break;
    }
  }
  return !context.refused;
}
}  // namespace PocketDaily::Screen
