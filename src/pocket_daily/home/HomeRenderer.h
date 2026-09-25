#pragma once

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
enum class RowSource : uint8_t { Reading, AppCards, DailyWord };

// What content exists right now; a source without content adds no page.
struct RowAvailability {
  bool book = false;      // an open book to continue
  bool appCards = false;  // at least one active app-authored card
};

// The profile's Home items resolved to sources, in page order. The device
// overview and the host preview both build their rows from this, so the order
// and fallback rules cannot drift between them. Returns the source count.
int homeRowSources(const DailyProfile::Profile& profile, const RowAvailability& available,
                   RowSource (&out)[DailyProfile::HOME_ITEM_CAP]);

struct Row {
  bool reading = false;
  bool pocket = false;
  bool mine = false;          // one of the companion's cards ("My cards")
  bool word = false;          // the firmware daily word
  bool hasImage = false;      // mine: the card carries an image
  const char* cardId = "";    // mine: identifies the card for Env::drawCardImage
  const char* project = "";   // item title
  const char* activity = "";  // item body text
};

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
  const char* noOpenBook = "No open book";
  const char* startReading = "Start reading";
  const char* study = "Today's Study";
  const char* myCards = "My Cards";
  const char* word = "Daily Word";
  const char* empty = "";
  const char* weather = "Weather";
  const char* noWeather = "";
  const char* weatherHint = "";
  const char* nextEvent = "Next event";
  const char* today = "Today";
  const char* library = "Library";
  const char* select = "Select";
  const char* sync = "Sync";
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
  // Draws the image of one of the companion's cards fitted into the box (shrunk,
  // never enlarged) and returns the height used; 0 when nothing was drawn.
  int (*drawCardImage)(void* context, GfxRenderer& renderer, const char* cardId, int x, int y, int width,
                       int height) = nullptr;
};

struct HomeView {
  bool isX3 = true;
  const Row* rows = nullptr;
  int count = 0;
  int selected = 0;
  Reading reading;
  const PocketDaily::Glance* glance = nullptr;  // required
  bool snapshotStale = false;
  DailyProfile::Profile profile = DailyProfile::defaults();
  Strings strings;
};

// Clears the framebuffer and draws the whole Home face.
void renderHome(GfxRenderer& renderer, const HomeView& view, const Env& env);

// Daily Brief / ambient glance (sleep frame when isSleep).
struct BriefView {
  bool isSleep = true;
  const char* headerMeta = nullptr;             // snapshot date/sync, nullptr when powered off
  const PocketDaily::Glance* glance = nullptr;  // required
  Reading reading;
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
