#include "HomeInputs.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <cstdio>
#include <string>

#include "CrossPointState.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "pocket_daily/ContentImageRenderer.h"
#include "pocket_daily/ContentPage.h"
#include "pocket_daily/ContentViewState.h"
#include "pocket_daily/PocketReadingShelf.h"
#include "pocket_daily/home/GlanceFormat.h"

namespace PocketDaily::Home {
namespace {
template <size_t N>
void copyBookText(char (&out)[N], const char* text) {
  // Keep the longest complete prefix; byte truncation can split a Korean
  // title/author and make the otherwise valid layout snapshot fail UTF-8 checks.
  const size_t length = strnlen(text, N - 1);
  memcpy(out, text, length);
  out[utf8SafeTruncateBuffer(out, static_cast<int>(length))] = '\0';
}
}  // namespace

DeskClock readDeskClock(const AppGlance::Snapshot& glance, const time_t now, const int16_t companionOffset) {
  DeskClock clock;
  if (now < AppGlance::MIN_EPOCH) return clock;
  struct tm local{};
  if (companionOffset != INT16_MIN || glance.savedEpoch) {
    const int16_t offset = companionOffset != INT16_MIN ? companionOffset : glance.utcOffsetMinutes;
    const time_t adjusted = now + offset * 60;
    if (!gmtime_r(&adjusted, &local)) return clock;
  } else if (!localtime_r(&now, &local)) {
    return clock;
  }
  clock.date = (local.tm_year + 1900) * 10000u + (local.tm_mon + 1) * 100u + local.tm_mday;
  clock.minute = local.tm_hour * 60 + local.tm_min;
  const char* line = tr(STR_POCKET_CLOCK_LINE_PROMISE);
  switch (local.tm_yday % 3) {
    case 1:
      line = tr(STR_POCKET_CLOCK_LINE_PAGES);
      break;
    case 2:
      line = tr(STR_POCKET_CLOCK_LINE_PAUSE);
      break;
    default:
      break;
  }
  snprintf(clock.line, sizeof(clock.line), "%s", line);
  return clock;
}

Strings deviceStrings() {
  Strings s;
  s.pocketDaily = tr(STR_POCKET_DAILY);
  s.continueReading = tr(STR_POCKET_CONTINUE_READING);
  s.noOpenBook = tr(STR_POCKET_READING_EMPTY);
  s.startReading = tr(STR_POCKET_READING_START);
  s.study = tr(STR_POCKET_STUDY);
  s.myCards = tr(STR_POCKET_MY_CARDS);
  s.word = tr(STR_POCKET_DAILY_WORD);
  s.empty = tr(STR_POCKET_EMPTY);
  s.weather = tr(STR_POCKET_WEATHER);
  s.noWeather = tr(STR_POCKET_NO_WEATHER);
  s.weatherHint = tr(STR_POCKET_WEATHER_HINT);
  s.nextEvent = tr(STR_POCKET_NEXT_EVENT);
  s.today = tr(STR_POCKET_TODAY);
  s.noEvents = tr(STR_POCKET_NO_EVENTS);
  s.home = tr(STR_POCKET_HOME);
  s.select = tr(STR_SELECT);
  s.library = tr(STR_LIBRARY);
  s.articles = tr(STR_ARTICLES);
  s.sync = tr(STR_POCKET_SYNC);
  s.recentBooks = tr(STR_POCKET_RECENT_BOOKS);
  s.views = tr(STR_POCKET_VIEWS);
  s.reading = tr(STR_POCKET_READING_LABEL);
  s.back = tr(STR_BACK);
  s.clock = tr(STR_CLOCK);
  s.calendar = tr(STR_POCKET_CALENDAR);
  s.weekdays = tr(STR_POCKET_WEEKDAYS);
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

std::string continueReadingPath() {
  if (!APP_STATE.openEpubPath.empty() && Storage.exists(APP_STATE.openEpubPath.c_str())) return APP_STATE.openEpubPath;
  for (const auto& book : RECENT_BOOKS.getBooks())
    if (!book.path.empty() && Storage.exists(book.path.c_str())) return book.path;
  return {};
}

const RecentBook* readOpenBook(OpenBook& out) {
  out.clear();
  const std::string openPath = continueReadingPath();
  if (openPath.empty()) return nullptr;

  out.valid = true;
  const RecentBook* found = nullptr;
  for (const auto& book : RECENT_BOOKS.getBooks()) {
    if (book.path != openPath) continue;
    copyBookText(out.title, book.title.c_str());
    copyBookText(out.author, book.author.c_str());
    found = &book;
    break;
  }
  if (!out.title[0]) {
    const char* path = openPath.c_str();
    const char* slash = strrchr(path, '/');
    copyBookText(out.title, slash ? slash + 1 : path);
  }

  // The seventh progress byte is a backwards-compatible whole-book percent.
  // Read it once per paint through HalStorage; older six-byte files simply keep
  // percent=-1 and still resume at their exact local spine/page position.
  if (!FsHelpers::hasEpubExtension(openPath)) return found;
  char progressPath[64];
  snprintf(progressPath, sizeof(progressPath), "/.crosspoint/epub_%u/progress.bin",
           (unsigned)std::hash<std::string>{}(openPath));
  HalFile progressFile;
  if (!Storage.openFileForRead("POCKET", progressPath, progressFile)) return found;
  uint8_t progressData[7];
  if (progressFile.read(progressData, sizeof(progressData)) == (int)sizeof(progressData) && progressData[6] <= 100)
    out.percent = (int8_t)progressData[6];
  return found;
}

bool resolveBookCoverPath(const RecentBook& book, char* path, const size_t capacity) {
  if (!path || !capacity) return false;
  path[0] = 0;
  if (book.coverBmpPath.empty()) return false;
  const auto accept = [&](const std::string& candidate) {
    if (candidate.empty() || candidate.size() >= capacity) return false;
    HalFile file;
    if (!Storage.openFileForRead("POCKET", candidate.c_str(), file) || !file.size() || file.size() > 131072)
      return false;
    memcpy(path, candidate.c_str(), candidate.size() + 1);
    return true;
  };
  std::string full = book.coverBmpPath;
  const auto thumb = full.rfind("/thumb_");
  if (thumb != std::string::npos) {
    full.replace(thumb, std::string::npos, "/cover.bmp");
    if (accept(full)) return true;
  }
  return accept(UITheme::getCoverThumbPath(book.coverBmpPath, UITheme::getInstance().getMetrics().homeCoverHeight));
}

uint8_t readRecentBookEntries(const RecentBook* (&entries)[RECENT_CAP]) {
  memset(entries, 0, sizeof(entries));
  auto selection = makeUniqueNoThrow<ReadingShelf::Selection>();
  if (!selection) return 0;
  ReadingShelf::load(*selection);
  uint8_t count = 0;
  const auto append = [&](const RecentBook& book) {
    if (count < RECENT_CAP && !book.path.empty() && book.path.size() < RECENT_PATH_BYTES &&
        Storage.exists(book.path.c_str()))
      entries[count++] = &book;
  };
  if (selection->automatic) {
    for (const auto& book : RECENT_BOOKS.getBooks()) append(book);
  } else {
    for (uint8_t i = 0; i < selection->count; ++i) {
      for (const auto& book : RECENT_BOOKS.getBooks()) {
        if (book.path == selection->paths[i]) {
          append(book);
          break;
        }
      }
    }
  }
  return count;
}

uint8_t readRecentBooks(BookSummary (&books)[RECENT_CAP], char (*paths)[RECENT_PATH_BYTES],
                        char (*covers)[RECENT_PATH_BYTES]) {
  for (auto& book : books) book.clear();
  if (paths) memset(paths, 0, RECENT_CAP * RECENT_PATH_BYTES);
  if (covers) memset(covers, 0, RECENT_CAP * RECENT_PATH_BYTES);
  uint8_t count = 0;
  const RecentBook* entries[RECENT_CAP]{};
  const auto entryCount = readRecentBookEntries(entries);
  for (uint8_t i = 0; i < entryCount; ++i) {
    const auto& recent = *entries[i];
    auto& book = books[count];
    book.valid = true;
    const char* slash = strrchr(recent.path.c_str(), '/');
    copyBookText(book.title, recent.title.empty() ? (slash ? slash + 1 : recent.path.c_str()) : recent.title.c_str());
    copyBookText(book.author, recent.author.c_str());
    if (paths) memcpy(paths[count], recent.path.c_str(), recent.path.size() + 1);
    // Preserve the exact cache association; never substitute another book's art.
    if (covers) resolveBookCoverPath(recent, covers[count], RECENT_PATH_BYTES);
    if (FsHelpers::hasEpubExtension(recent.path)) {
      char progressPath[64];
      snprintf(progressPath, sizeof(progressPath), "/.crosspoint/epub_%u/progress.bin",
               static_cast<unsigned>(std::hash<std::string>{}(recent.path)));
      HalFile file;
      uint8_t bytes[7];
      if (Storage.openFileForRead("POCKET", progressPath, file) && file.read(bytes, sizeof(bytes)) == sizeof(bytes) &&
          bytes[6] <= 100)
        book.percent = static_cast<int8_t>(bytes[6]);
    }
    ++count;
  }
  return count;
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
