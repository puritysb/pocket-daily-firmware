#pragma once
//
// AgentDashboardActivity — entry point for the AgentDeck "Decision Card" mode.
//
// M1: scaffold + AgentLog SD diagnostic channel.
// M2 (this commit): NETWORK layer. Brings up WiFi (mirrors CalibreConnect),
//   discovers the AgentDeck daemon over mDNS (_agentdeck._tcp), connects to it
//   over WebSocket, registers as an eink-device, and renders live connection +
//   agent state. Display-only — button approve/deny lands in M3 (the outbound
//   builders are already ported in agentdeck/agent_commands.*).
// M3: render awaiting Decision Cards + approve/deny via the physical buttons.
// M4 (this commit): full-screen Decision Card view — the product grammar is
//   "one screen, one question, ≤4 choices, one physical press". When any
//   session needs a decision, the card auto-surfaces from Overview and maps
//   the four front buttons directly to [Later][…choices…]; long prompts and
//   >3-option prompts degrade to the cursor grammar. Dismissed prompts are
//   remembered by content signature so a card never re-surfaces unchanged.
//
// Concurrency: net is serviced cooperatively from loop() on the main task — no
// FreeRTOS network task. render() runs on the separate render task and reads the
// mutex-guarded g_state.
//
#include <memory>
#include <string>

#include "activities/Activity.h"
#include "agentdeck/agent_state.h"
#include "agentdeck/attention_contract.h"
#include "agentdeck/deck_store.h"

class AgentDashboardActivity final : public Activity {
 public:
  explicit AgentDashboardActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("AgentDashboard", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  // Keep the main loop tight + the radio awake while we're servicing the daemon.
  bool skipLoopDelay() override { return dashState != DashState::WifiSelection; }
  bool preventAutoSleep() override { return dashState != DashState::WifiSelection; }

 private:
  // WifiJoining = background STA join with saved credentials (no picker UI —
  // the Face renders immediately and the join is just a status line). The
  // interactive WifiSelection picker is only pushed when there are no saved
  // credentials or the background join times out.
  enum class DashState : uint8_t { WifiSelection, WifiJoining, Discovering, Connecting, Connected };

  // One session needing attention. Collected from sessions_list (or the focused
  // state_update fallback) and classified by the shared attention contract;
  // observed sessions without a requestId are deliberately display-only.
  struct AwaitingItem {
    char sid[64];  // full/prefixed session UUID — see SessionInfo::id sizing
    char question[200];
    char requestId[40];   // present → observed gate (permission_decision)
    uint8_t optionCount;  // meaningful only when optionSessionId == sid
    AgentDeck::AttentionMode attentionMode;
  };

  // Compact per-session row for the Overview / Mission Control list. The bounded
  // activity copy is intentionally large enough for 2–3 visible lines; at the
  // 10-session cap this array adds 1,920 bytes to the render-task stack.
  struct OverviewRow {
    char sid[64];
    char project[40];
    char agentType[16];
    char controlMode[12];
    char state[20];
    char activity[AgentDeck::SESSION_ACTIVITY_CAP];
    bool awaiting;
  };

  // Which screen the Connected dashboard is showing. Overview is home; OK opens a
  // session's read-only Detail (timeline + inline options as a fallback grammar).
  // Card is the primary decision surface: it auto-surfaces from Overview when any
  // session needs attention, and maps the front buttons to the choices directly.
  enum class ViewMode : uint8_t { Overview, Detail, Card };

  void onWifiSelectionComplete(bool connected);
  void launchWifiPicker();
  void startNetworking();
  void sendClientRegister();
  void sendDeviceInfo();
  uint32_t computeStateSignature() const;

  // Snapshot one selected awaiting session. Detail never needs the old triage
  // array, so this avoids placing six large question buffers on the C3 stack.
  bool findAwaiting(const char* sid, AwaitingItem& out) const;
  // First session (overview order) whose attention state warrants a card and
  // whose content signature hasn't been dismissed. Returns false when quiet.
  bool firstCardCandidate(AwaitingItem& out) const;
  // Content signature of a prompt (sid + question + requestId + option shape).
  // A dismissed card only re-surfaces when this changes — same honesty rule as
  // the state signature: content decides, not time.
  static uint32_t awaitingSignature(const AwaitingItem& it);
  bool isDismissed(uint32_t sig) const;
  // Auto-surface / auto-resolve the Card view. Runs from loop() while Connected.
  void serviceCard();
  // True when the card can bind choices directly to front buttons (softkey
  // grammar); false falls back to the cursor grammar inside the card.
  static bool cardUsesSoftkeys(AgentDeck::AttentionMode mode, uint8_t optionCount);
  // Fill out[] with all alive sessions (overview order); returns the count.
  int collectOverview(OverviewRow* out, int cap) const;
  // Shared responsive paper-card component used by X3 portrait and X4
  // landscape overview grids. Geometry comes from eink_dashboard_layout.h.
  void drawOverviewCard(const OverviewRow& row, int x, int y, int w, int h, bool selected) const;
  void handleButtons();
  bool applyDecision(const AwaitingItem& it, int optionCursor);
  // fromCache renders the persisted deck (display-only, no selection/Open) with
  // an "as of" sync-age line derived from asOfEpoch. Live renders pass false/0.
  void renderOverview(const OverviewRow* rows, int n, int awaitingCount, bool fromCache, uint32_t asOfEpoch);
  void renderDetail();
  void renderCard();
  // The frame the panel holds through a timed deep sleep: weather + provider
  // quota + work wrap-up from the feed's glance block, with ABSOLUTE times only
  // ("Synced HH:MM · next ~HH:MM") — a frozen frame must stay true without a
  // repaint. Every kGlanceFullRefreshEvery-th paint uses FULL_REFRESH to clear
  // accumulated fast-refresh ghosting.
  void renderSleepGlance();
  // Branded header (AgentDeck mark + title) shared by every Connected screen.
  void drawBrandedHeader(const char* title, const char* subtitle) const;
  // LIMITS footer — 5H/7D quota gauges. Renders only when the hub supplies usage
  // (fiveHourPercent/sevenDayPercent >= 0); hidden otherwise. Returns the y it
  // started at (so callers know the content ceiling) or pageHeight if nothing drawn.
  int drawLimitsFooter() const;

  // The daemon port is dynamic (9120, falling back up to 9139), so a cached
  // ip:port goes stale across a daemon restart. If a connect attempt doesn't
  // succeed within this window, drop back to Discovering and re-resolve via mDNS
  // instead of hammering the old port. See feedback_daemon_port_flexibility.
  static constexpr uint32_t kConnectTimeoutMs = 10000;
  // Background STA join budget before falling back to the interactive picker.
  static constexpr uint32_t kWifiJoinTimeoutMs = 20000;
  static constexpr uint32_t kExitHoldMs = 700;          // hold Back this long = exit while a card is up
  static constexpr uint32_t kDecisionCooldownMs = 400;  // debounce a sent decision
  // A connection that survives longer than this is "healthy": on drop we retry the
  // SAME endpoint (transient drop / daemon restart). A connection that dies sooner
  // is a flaky/duplicate daemon — re-resolve via mDNS to try a different advertiser
  // instead of hammering it (multiple daemons on the LAN round-robin otherwise).
  static constexpr uint32_t kHealthyUptimeMs = 8000;
  // Discovery normally resolves quickly via UDP beacons or mDNS. After this
  // window, keep scanning but switch the screen from generic "searching" to an
  // actionable "AgentDeck not running" state.
  static constexpr uint32_t kDiscoveryNotFoundMs = 12000;
  // Interval between state-signature checks; caps repaints at ≤2/sec and keeps the
  // skipLoopDelay() loop from re-hashing all shared state under g_stateMutex every spin.
  static constexpr uint32_t kSigCheckIntervalMs = 500;

  // ── Link-flap cool-down ──
  // Consecutive short-lived connections (< kHealthyUptimeMs). At the
  // threshold, connecting pauses for kFlapCooldownMs while the Face keeps
  // rendering the last-known deck: a marginal-RF link must not hammer
  // discovery+connect every second (it churns the radio AND resonates with
  // the daemon's per-connect burst cost).
  int flapShortLived = 0;
  uint32_t nextConnectAllowedMs = 0;
  static constexpr int kFlapThreshold = 3;
  static constexpr uint32_t kFlapCooldownMs = 30000;

  DashState dashState = DashState::WifiSelection;
  std::string localIp;
  uint32_t wifiJoinStartMs = 0;
  char joiningSsid[33] = {0};  // SSID of the in-progress background join (status line)

  // ── WiFi OTA (AgentDeck daemon → device) ──
  bool otaFlashNotice = false;  // render paints the full-screen "do not power off" notice
  int otaPctBucket = -1;        // last painted receive-progress bucket (5% steps)
  bool exitRequested = false;
  bool registered = false;
  uint32_t lastSignature = 0;
  uint32_t lastSigCheckMs = 0;
  uint32_t connectStartMs = 0;
  uint32_t discoveryStartMs = 0;
  bool discoveryNoticeShown = false;
  uint32_t lastConnectedMs = 0;  // when we last reached Connected (for healthy-vs-flaky drop)

  // Screen navigation
  ViewMode viewMode = ViewMode::Overview;
  int overviewCursor = 0;      // selected row in the Overview list
  int overviewTop = 0;         // first visible row (scroll window)
  int detailScroll = 0;        // first visible content line in Detail
  int detailMaxScroll = 0;     // set by renderDetail; lets handleButtons know "at bottom"
  char selectedSid[64] = {0};  // session opened into Card/Detail (re-resolved each frame)
  // Installed SD CJK font id (the reader's font when it's an SD font) so CJK text
  // renders instead of □; 0 when none — Latin-only built-in UI fonts have no CJK.
  int cjkFontId = 0;

  // Returns the SD CJK font id for text containing CJK (loading its glyphs), else
  // the given UI font id. Use the return value for both measuring and drawing.
  int fontForText(int uiFontId, const char* text) const;

  // Load the bundled Noto Sans KR (OFL) font shipped on the SD at /.fonts/ so
  // Korean renders without the user installing a font. Loaded once; returns its
  // font id, or 0 when the file isn't present.
  int loadKoreanFont();

  // Decision-card / triage cursors
  int triageIndex = 0;       // which awaiting session is shown
  int optionCursor = 0;      // which option is highlighted (option prompts)
  uint32_t backPressMs = 0;  // Back press timestamp for short(deny)/long(exit/back)
  uint32_t lastDecisionMs = 0;

  // ── Card view state ──
  char cardSid[64] = {0};          // session the Card is showing
  uint32_t cardSig = 0;            // signature of the prompt the Card is showing
  bool cardFocusSent = false;      // focus_session sent for this card (WaitingForOptions)
  // Card→Detail transitions consume a raw front-button PRESS; the mapped RELEASE
  // of that same press would otherwise land in Detail's Confirm handler and could
  // fire an inline decision (cooldown only covers short presses). Swallow it.
  bool swallowConfirmRelease = false;
  uint32_t lastUserInputMs = 0;    // any button press — suppresses auto-surface mid-navigation
  // Recently dismissed prompt signatures (ring). Multiple sessions can be
  // awaiting at once; a single slot would resurrect A when B is dismissed.
  static constexpr int kDismissedCap = 8;
  uint32_t dismissedSigs[kDismissedCap] = {0};
  uint8_t dismissedHead = 0;
  // Don't auto-surface while the user is actively pressing buttons.
  static constexpr uint32_t kAutoSurfaceQuietMs = 2500;

  // ── M5.5 Deck persistence ──
  // The persisted deck doubles as the offline fallback: loaded from SD in
  // onEnter (so boot renders the last-synced deck before any connection) and
  // refreshed in place whenever the live deck's content signature changes.
  // Guarded by g_stateMutex: written on the loop task, read on the render task.
  // Heap (unique_ptr), not a member array — ~3.5 KB must not sit on the C3 stack.
  std::unique_ptr<AgentDeck::DeckStore::Snapshot> cachedDeck;
  uint32_t lastDeckSig = 0;      // content signature of the last persisted deck
  uint32_t lastDeckSaveMs = 0;   // SD-write throttle
  bool clockSynced = false;      // SNTP landed — repaint once so "as of" gains an age
  static constexpr uint32_t kDeckSaveIntervalMs = 5000;

  // Persist the live deck to SD when its content changed (throttled; loop task).
  void serviceDeckPersist();
  // Copy the cached deck into overview rows for the offline Face. Returns count.
  int buildRowsFromCache(OverviewRow* out, int cap) const;
  // Current unix-seconds estimate: NTP clock first, daemon-clock estimate else 0.
  static uint32_t bestEpochNow();

  // ── M6 pull-sync power ladder ──
  // Timer wake with the battery cadence enabled: join Wi-Fi, sync once over
  // HTTP (GET /feed + outbox push), repaint the Face, then deep-sleep again
  // with the timer armed. Any button press cancels into the interactive WS
  // flow; USB power disables the cadence entirely (docked = live mode).
  bool pullMode = false;
  bool pullSynced = false;
  bool pullEndpointTried = false;   // cached-endpoint fast path attempted
  bool timedSleepImminent = false;  // final Face paint says "sleeping"
  uint32_t pullSyncedAtMs = 0;
  uint32_t pullNextSec = 0;  // daemon's nextPullSec hint (0 → default)
  // Conditional pull: deckSig of the last applied feed (seeded from the SD deck
  // cache at onEnter, refreshed on every full feed). Echoed as `?sig=` so an
  // unchanged deck costs one tiny response. Empty = always pull the full feed.
  char lastFeedSig[12] = {0};
  // Sleep-frame wall times (daemon-local "HH:MM"), computed in beginTimedSleep
  // from the feed's serverHm; empty when no pull has anchored wall time yet.
  char sleepSyncHm[6] = {0};
  char sleepNextHm[6] = {0};
  uint32_t sleepForSec = 0;  // fallback "sleeping ~Nm" when no wall time known
  static constexpr uint32_t kGlanceFullRefreshEvery = 8;  // ghost-clearing cadence
  uint32_t enterMs = 0;      // onEnter millis — pull budget / idle anchor
  static constexpr uint32_t kPullLingerMs = 20000;   // user-presence window after a sync
  static constexpr uint32_t kPullBudgetMs = 60000;   // whole wake budget before sleeping unsynced
  static constexpr uint32_t kPullDefaultSec = 3600;  // no daemon hint → hourly
  static constexpr uint32_t kPullActiveSec = 900;    // sessions mid-turn → 15 min cadence
  static constexpr uint32_t kPullMinSec = 300;       // clamp against a degenerate hint
  static constexpr uint32_t kIdleToCadenceMs = 5 * 60 * 1000;  // interactive → cadence handoff
  // One HTTP sync against the given endpoint; updates the pull state on success.
  bool attemptPullSync(const char* ip, uint16_t port, const char* token);
  // Pull-mode step, run from loop(): cached-endpoint fast path + sleep decisions.
  void servicePullSync();
  // Interactive-mode idle → timed sleep handoff (cadence setting on, on battery).
  void serviceIdleCadence();
  // Final deck persist + honest "sleeping" paint, then timed deep sleep.
  void beginTimedSleep(uint32_t seconds);
};
