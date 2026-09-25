#pragma once
//
// PocketDailyActivity — offline-first Pocket reader shell.
//
// Local EPUB state, the companion's cards, the firmware daily word and the
// app-provided weather/events glance (docs/pocket-glance-v1.md) are the
// product surface. Pocket Daily never raises the radio itself: the companion
// reaches the reader through the Sync button (Pocket Nearby Sync).
//
// Concurrency: input runs on the loop task and paints on the render task. The
// render lock (RenderLock) guards the few members both touch after onEnter.
//
#include <cstring>

#include "activities/Activity.h"
#include "pocket_daily/ContentViewState.h"
#include "pocket_daily/PocketGlance.h"
#include "pocket_daily/home/HomeRenderer.h"
#include "pocket_daily/models.h"

class PocketDailyActivity final : public Activity {
 public:
  explicit PocketDailyActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("PocketDaily", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Powering the device down from Pocket Daily leaves the Daily Brief on the
  // panel instead of the generic sleep screen — the retained frame stays useful.
  bool paintSleepFrame() override;

 private:
  // Compact row for the local book, one of the companion's cards or the daily
  // word. Fixed buffers keep the render-task stack bounded on the no-PSRAM C3.
  struct OverviewRow {
    char sid[72];  // "local:reading" or the card id
    char project[40];
    char activity[192];
    bool pocket;
    bool reading;
    bool mine;      // one of the companion's cards
    bool word;      // the firmware daily word
    bool hasImage;  // mine: the card carries an image
  };

  // Local reading + three card slots. The profile orders these sources; a
  // later source is dropped when full.
  static constexpr int kOverviewCap = 1 + PocketDaily::CARD_CAP;
  // Scratch lives in the heap-allocated Activity object, not either task stack.
  // Separate render/loop copies prevent cross-task races.
  OverviewRow renderRows[kOverviewCap] = {};
  OverviewRow inputRows[kOverviewCap] = {};
  // The app-provided glance, loaded from SD once in onEnter (~800 B in this
  // heap object, never a static). Only the render task reads it afterwards; it
  // rolls the snapshot forward when the local day has changed since it was
  // saved.
  PocketDaily::AppGlance::Snapshot glanceSnapshot{};
  // The Daily Brief and retained sleep frame show at most one carried learning
  // item. Keep the ~700 B card in the heap-allocated activity object, never on
  // the 12 KB render stack.
  PocketDaily::Card renderPocketSnapshot{};
  // Firmware-authored Japanese daily word. Written on the loop task under the
  // render lock; the render task reads it while holding that lock.
  PocketDaily::Card localStudyCard{};
  // Immutable after onEnter, read by loop/render; released after render stops.
  // <=2400B only when app content is active, not a permanent radio allocation.
  PocketDaily::Content::ContentViewState appContent;
  // Returns the height drawn; 0 when the card has no drawable image.
  int drawAppCardImage(const char* cardId, int x, int y, int width, int height) const;
  uint16_t localStudyOffset = 0;
  uint32_t localStudyPackVersion = 0;
  uint32_t localStudyPackRecordCount = 0;
  struct ReadingSummary {
    char title[96];
    char author[80];
    // Resolved SD thumbnail path for the currently-open book. Keeping it in
    // the render snapshot lets Pocket paint the real cached cover without
    // loading/parsing the EPUB or allocating a second RecentBook vector.
    char coverBmpPath[160];
    int8_t percent;
    bool valid;
    void clear() {
      memset(this, 0, sizeof(*this));
      percent = -1;
    }
  } renderReadingSnapshot{};

  // Render-task-only snapshot of the personal plane. It reads fixed data from
  // RAM/SD once at the start of a paint and feeds both interactive and retained
  // layouts without per-section allocations.
  void preparePersonalSnapshot();
  bool drawReadingCover(int x, int y, int width, int height) const;
  void buildLocalStudyCard();
  // Best wall-clock estimate for the daily word: the system clock when set,
  // else the glance's compose time (a lower bound), else 0.
  uint32_t studyEpoch() const;

  // Overview is Home; Card shows one of the companion's cards or the daily word.
  enum class ViewMode : uint8_t { Overview, Card };

  bool findPocketCard(const char* cardId, PocketDaily::Card& out) const;
  // Fill out[] with the Home carousel rows (profile order); returns the count.
  int collectOverview(OverviewRow* out, int cap) const;
  // Shared Home painter inputs (pocket_daily/home/HomeRenderer).
  static PocketDaily::Home::Strings homeStrings();
  PocketDaily::Home::Env homeEnv() const;
  void handleButtons();
  bool applyPocketChoice(const PocketDaily::Card& card, int optionCursor);
  bool closePocketCard(const PocketDaily::Card& card);
  void renderOverview(const OverviewRow* rows, int n);
  void renderPocketCard(const PocketDaily::Card& card);
  // Why the Daily Brief is being painted: the ambient face (no book or card
  // row to page through) or the retained powered-off frame.
  enum class GlanceReason : uint8_t { Ambient, PoweredOff };
  // Reading, a carried card, weather and today's schedule. Times are ABSOLUTE
  // because a retained e-ink frame must stay true without a repaint.
  void renderGlance(GlanceReason reason);
  void drawBrandedHeader(const char* title, const char* subtitle) const;

  static constexpr uint32_t kDecisionCooldownMs = 400;  // debounce a card choice

  bool exitRequested = false;
  ViewMode viewMode = ViewMode::Overview;
  int overviewCursor = 0;  // selected Reading/Study item in the Home carousel
  // Returns a script-appropriate SD CJK font id when available, else the given
  // UI font. The shared selector validates real glyph coverage, which matters
  // for Japanese: the old dashboard-local Hangul font accepted Kana/Han lines
  // and then rendered them blank.
  int fontForText(int uiFontId, const char* text) const;

  uint32_t backPressMs = 0;  // guards a stale Back release from the previous activity
  uint32_t lastDecisionMs = 0;
  char cardSid[72] = {0};  // card the Card view is showing

  // Which Daily Brief variant render() should paint. PoweredOff means this
  // frame is the one the panel keeps through sleep.
  GlanceReason glanceReason = GlanceReason::Ambient;
  // True while the Ambient Daily Brief is the face on screen. Written by
  // render() (render task), read by handleButtons() (loop task) to give Confirm
  // its "resume reading" meaning only when that face is displayed — a stale
  // read is benign (one inert or late press), so no lock.
  bool ambientGlanceShown = false;
  // Confirm on the Daily Brief: onExit restarts into the reader
  // (silentRestartToReader) instead of Home.
  bool exitToReader = false;
  bool exitToNearbySync = false;
  bool sleepFramePending = false;  // render() must paint the sleep Daily Brief
};
