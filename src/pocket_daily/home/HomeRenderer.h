#include "pocket_daily/PocketAgenda.h"
#pragma once

#include <cstddef>
#include <cstdint>

#include "pocket_daily/PocketProfile.h"
#include "pocket_daily/home/HomeDrawing.h"
#include "pocket_daily/models.h"

class GfxRenderer;

// Pocket Daily Home and Daily Brief painters shared by the device activity and
// the host preview (P1-3, docs/pocket-profile-v1.md). They draw from a plain
// view model; state, radio, SD and theme access stay with the caller, which
// supplies them through Env. Neither function presents the framebuffer.
namespace PocketDaily::Home {
// Where Home pages come from. Study resolves to the active app cards, or to
// the firmware daily word when there are none and the profile keeps it. The
// retired `provider` and `monitor` profile items (AgentDeck daemon data) still
// parse but resolve to no source.
enum class RowSource : uint8_t { Reading, AppCards, DailyWord, DeskClock, RecentBooks, Calendar };

// Reading retains its empty-state page; optional sources need content.
struct RowAvailability {
  bool book = false;      // an open book to continue
  bool appCards = false;  // at least one active app-authored card
};

// The profile's Home items resolved to sources, in page order. The device
// overview and the host preview both build their rows from this, so the order
// and fallback rules cannot drift between them. Returns the source count.
int homeRowSources(const DailyProfile::Profile& profile, const RowAvailability& available,
                   RowSource (&out)[DailyProfile::HOME_ITEM_CAP]);

// Home row text for one card, as the device composes it: the card title (or
// "POCKET"), and its question followed by " - " and its context when the card
// is not the firmware daily word (module "local"), the context differs from the
// question and the whole fits `activityCap` bytes (UTF-8 is never cut).
const char* cardRowTitle(const PocketDaily::Card& card);
void cardRowActivity(const PocketDaily::Card& card, char* activity, size_t activityCap);

// The Daily Brief's weather section label: the place (else `fallback`), then
// " · MM.DD" and " SAVED" when the snapshot is stale. Shared with the Sync
// screen presenter, which checks the label's font coverage before drawing.
inline constexpr size_t BRIEF_WEATHER_LABEL_BYTES = 56;
void briefWeatherLabel(char* out, size_t cap, const PocketDaily::Weather& weather, const char* fallback, bool stale);

struct Row {
  bool reading = false;
  bool pocket = false;
  bool mine = false;          // one of the companion's cards ("My cards")
  bool word = false;          // the firmware daily word
  bool hasImage = false;      // mine: the card carries an image
  const char* cardId = "";    // mine: identifies the card for Env::drawCardImage
  const char* project = "";   // item title
  const char* activity = "";  // item body text
  bool clockPage = false;
  int8_t shelfIndex = -1;
  bool calendarPage = false;
};

enum class Section : uint8_t { Reading, Study, Clock, Calendar };
constexpr Section homeSection(bool reading, bool shelf, bool clock, bool calendar) {
  return calendar ? Section::Calendar : clock ? Section::Clock : (reading || shelf) ? Section::Reading : Section::Study;
}
// Same-section navigation wraps without crossing into another content type.
int stepHomeSection(const uint8_t* groups, int count, int selected, int direction);
int homeSectionStarts(const uint8_t* groups, int count, uint8_t* starts, int capacity);

inline constexpr uint8_t RECENT_CAP = 4;
struct BookSummary {
  char title[96]{};
  char author[80]{};
  int8_t percent = -1;
  bool valid = false;
  void clear() { *this = {}; }
};
// Pure calendar geometry; Monday is column zero. Zero date means unset.
int calendarDays(int year, int month);
int calendarWeekday(int year, int month);
uint32_t calendarMonth(uint32_t date, int offset);
struct Reading {
  bool valid = false;
  const char* title = "";
  const char* author = "";
  int percent = -1;  // -1 when unknown
};

// Resolved UI strings (the device passes tr() values, the host English).
struct Strings {
  const char* pocketDaily = "Pocket Daily";
  const char* continueReading = "Continue Reading";
  const char* noOpenBook = "No book open";
  const char* startReading = "Choose a book in Library";
  const char* study = "Today's Study";
  const char* myCards = "My Cards";
  const char* word = "Daily Word";
  const char* empty = "";
  const char* weather = "Weather";
  const char* noWeather = "No saved weather";
  const char* weatherHint = "Set up weather in the app, then sync";
  const char* nextEvent = "Next event";
  const char* today = "Today";
  const char* noEvents = "No saved events";
  const char* home = "Home";
  const char* select = "Select";
  const char* library = "Library";
  const char* articles = "Articles";
  const char* sync = "Sync";
  const char* recentBooks = "Recent books";
  const char* views = "Views";
  const char* reading = "Reading";
  const char* back = "Back";
  const char* clock = "Clock";
  const char* calendar = "Calendar";
  const char* weekdays = "Mon|Tue|Wed|Thu|Fri|Sat|Sun";
};

struct Metrics {
  int topPadding = 0;
  int headerHeight = 0;
  int verticalSpacing = 0;
  int contentSidePadding = 0;
  int sideButtonHintsWidth = 0;
};

struct Env {
  Metrics metrics;
  HomeDraw::FontResolver text;    // body text (device: full CJK coverage required)
  HomeDraw::FontResolver labels;  // short control labels (device: partial allowed)
  void* context = nullptr;
  void (*drawHeader)(void* context, const GfxRenderer& renderer, int x, int y, int width, int height, const char* title,
                     const char* subtitle) = nullptr;
  bool (*drawCover)(void* context, GfxRenderer& renderer, int x, int y, int width, int height) = nullptr;
  bool (*drawRecentCover)(void* context, GfxRenderer& renderer, unsigned index, int x, int y, int width,
                          int height) = nullptr;
  // Draws the image of one of the companion's cards fitted into the box (shrunk,
  // never enlarged) and returns the height used; 0 when nothing was drawn.
  int (*drawCardImage)(void* context, GfxRenderer& renderer, const char* cardId, int x, int y, int width,
                       int height) = nullptr;
};

struct DeskClock {
  uint32_t date = 0;       // YYYYMMDD in the clock's local zone
  uint16_t minute = 1440;  // unset
  char line[96]{};
  bool hideColon = false;
  uint16_t foldingFromMinute = 1440;  // awake device transition only
  uint8_t second = 60;                // unset outside the seconds trial
  uint8_t foldingFromSecond = 60;     // awake device transition only
};

struct ClockDetail {
  const char* label;
  char text[112]{};
};

ClockDetail clockDetail(const DailyProfile::Profile& profile, const DeskClock& clock, const Glance& glance,
                        const Strings& strings);

struct HomeView {
  bool isX3 = true;
  const Row* rows = nullptr;
  int count = 0;
  int selected = 0;
  bool choosingView = false;
  uint8_t viewChoice = 0;
  Reading reading;
  const BookSummary* recentBooks = nullptr;
  uint8_t recentCount = 0;
  int8_t calendarOffset = 0;
  bool calendarBrowsing = false;
  uint32_t calendarSelection = 0;
  const Agenda::Snapshot* agenda = nullptr;
  const PocketDaily::Glance* glance = nullptr;  // required
  bool snapshotStale = false;
  DailyProfile::Profile profile = DailyProfile::defaults();
  Strings strings;
  DeskClock clock;
};

// Clears the framebuffer and draws the whole Home face.
void renderHome(GfxRenderer& renderer, const HomeView& view, const Env& env);

// Daily Brief / ambient glance (sleep frame when isSleep).
struct BriefView {
  bool isSleep = true;
  int contentTopInset = 0;                      // reserved for the physical wake cue when present
  const char* headerMeta = nullptr;             // snapshot date/sync, nullptr when powered off
  const PocketDaily::Glance* glance = nullptr;  // required
  Reading reading;
  bool fullScreenCover = false;
  bool sleepCover = true;                         // SETTINGS.pocketDailySleepCover
  const PocketDaily::Card* pocketCard = nullptr;  // required; empty cardId = none
  const PocketDaily::Card* pinnedCard = nullptr;  // first of the companion's cards, or nullptr
  bool pinnedHasImage = false;
  bool snapshotStale = false;
  const char* status = "";  // bottom line, absolute times only
  DailyProfile::Profile profile = DailyProfile::defaults();
  Strings strings;
};

// Clears the framebuffer and draws the Daily Brief including its status line;
// the caller adds the wake cue or button hints and presents the frame.
void renderBrief(GfxRenderer& renderer, const BriefView& view, const Env& env);
}  // namespace PocketDaily::Home
