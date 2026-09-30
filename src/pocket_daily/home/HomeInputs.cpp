#include "HomeInputs.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>
#include <string>

#include "CrossPointState.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "pocket_daily/ContentImageRenderer.h"
#include "pocket_daily/ContentPage.h"
#include "pocket_daily/ContentViewState.h"
#include "pocket_daily/home/GlanceFormat.h"

namespace PocketDaily::Home {

Strings deviceStrings() {
  Strings s;
  s.pocketDaily = tr(STR_POCKET_DAILY);
  s.continueReading = tr(STR_POCKET_CONTINUE_READING);
  s.noOpenBook = tr(STR_NO_OPEN_BOOK);
  s.startReading = tr(STR_START_READING);
  s.study = tr(STR_POCKET_STUDY);
  s.myCards = tr(STR_POCKET_MY_CARDS);
  s.word = tr(STR_POCKET_DAILY_WORD);
  s.empty = tr(STR_POCKET_EMPTY);
  s.weather = tr(STR_POCKET_WEATHER);
  s.noWeather = tr(STR_POCKET_NO_WEATHER);
  s.weatherHint = tr(STR_POCKET_WEATHER_HINT);
  s.nextEvent = tr(STR_POCKET_NEXT_EVENT);
  s.today = tr(STR_POCKET_TODAY);
  s.home = tr(STR_POCKET_HOME);
  s.select = tr(STR_SELECT);
  s.articles = tr(STR_ARTICLES);
  s.sync = tr(STR_POCKET_SYNC);
  return s;
}

Metrics deviceMetrics() {
  const auto& m = UITheme::getInstance().getMetrics();
  return {m.topPadding, m.headerHeight, m.verticalSpacing, m.contentSidePadding, m.sideButtonHintsWidth};
}

void rollGlanceToToday(PocketDaily::AppGlance::Snapshot& snapshot, const time_t now) {
  if (!snapshot.savedEpoch || now < static_cast<time_t>(PocketDaily::AppGlance::MIN_EPOCH)) return;
  char savedIso[11];
  char todayIso[11];
  if (GlanceFormat::formatLocalIsoDate(savedIso, sizeof(savedIso), snapshot.savedEpoch, snapshot.utcOffsetMinutes) &&
      GlanceFormat::formatLocalIsoDate(todayIso, sizeof(todayIso), now, snapshot.utcOffsetMinutes))
    GlanceFormat::rollToLocalDay(snapshot.glance, savedIso, todayIso);
}

const RecentBook* readOpenBook(OpenBook& out) {
  out.clear();
  if (APP_STATE.openEpubPath.empty()) return nullptr;

  out.valid = true;
  const RecentBook* found = nullptr;
  for (const auto& book : RECENT_BOOKS.getBooks()) {
    if (book.path != APP_STATE.openEpubPath) continue;
    snprintf(out.title, sizeof(out.title), "%s", book.title.c_str());
    snprintf(out.author, sizeof(out.author), "%s", book.author.c_str());
    found = &book;
    break;
  }
  if (!out.title[0]) {
    const char* path = APP_STATE.openEpubPath.c_str();
    const char* slash = strrchr(path, '/');
    snprintf(out.title, sizeof(out.title), "%s", slash ? slash + 1 : path);
  }

  // The seventh progress byte is a backwards-compatible whole-book percent.
  // Read it once per paint through HalStorage; older six-byte files simply keep
  // percent=-1 and still resume at their exact local spine/page position.
  if (!FsHelpers::hasEpubExtension(APP_STATE.openEpubPath)) return found;
  char progressPath[64];
  snprintf(progressPath, sizeof(progressPath), "/.crosspoint/epub_%u/progress.bin",
           (unsigned)std::hash<std::string>{}(APP_STATE.openEpubPath));
  HalFile progressFile;
  if (!Storage.openFileForRead("POCKET", progressPath, progressFile)) return found;
  uint8_t progressData[7];
  if (progressFile.read(progressData, sizeof(progressData)) == (int)sizeof(progressData) && progressData[6] <= 100)
    out.percent = (int8_t)progressData[6];
  return found;
}

int drawContentCardImage(GfxRenderer& renderer, const PocketDaily::Content::ContentViewState& content,
                         const char* cardId, int x, int y, int width, int height) {
  const auto* appCards = content.cards();
  if (!appCards || !content.revision()[0] || !cardId || width <= 0 || height <= 0) return 0;
  for (uint8_t i = 0; i < appCards->count; ++i) {
    const auto& card = appCards->cards[i];
    if (!card.imagePath[0] || strcmp(card.card.cardId, cardId) != 0) continue;
    char path[160];
    snprintf(path, sizeof(path), "%s/%s/%s", PocketDaily::Content::CONTENT_ROOT, content.revision(), card.imagePath);
    HalFile file = Storage.open(path, O_RDONLY);
    if (!file) {
      LOG_ERR("CONTENT", "Card image unavailable");
      return 0;
    }
    const PocketDaily::Content::ManifestSource source{
        &file, file.size(), [](void* context, size_t offset, uint8_t* bytes, size_t count) {
          HalSystem::feedWatchdogIfRegistered();
          auto& file = *static_cast<HalFile*>(context);
          return file.seek(offset) && file.read(bytes, count) == static_cast<int>(count);
        }};
    const int drawn = PocketDaily::Content::contentImageHeight(renderer, source, x, y, width, height);
    return drawn > 0 && PocketDaily::Content::drawContentImage(renderer, source, x, y, width, height) ? drawn : 0;
  }
  return 0;
}

}  // namespace PocketDaily::Home
