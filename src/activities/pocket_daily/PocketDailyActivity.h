#pragma once
//
// PocketDailyActivity — offline-first Pocket reader shell.
//
// Local EPUB state and the SD-backed Pocket pool are the product surface.
// AgentDeck is an optional background authoring source; Wi-Fi and live sessions
// never gate entry or replace carried content with a connection screen.
//
// Concurrency: net is serviced cooperatively from loop() on the main task — no
// FreeRTOS network task. render() runs on the separate render task and reads the
// mutex-guarded g_state.
//
#include <cstring>
#include <memory>
#include <string>

#include "activities/Activity.h"
#include "agentdeck/agent_state.h"
#include "agentdeck/attention_contract.h"
#include "agentdeck/deck_store.h"
#include "agentdeck/endpoint_candidates.h"
#include "agentdeck/ota_ws_receiver.h"
#include "pocket_daily/models.h"

class PocketDailyActivity final : public Activity {
 public:
  explicit PocketDailyActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("PocketDaily", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Powering the device down from the dashboard leaves the glance on the panel
  // instead of the generic sleep screen — the retained frame stays useful.
  bool paintSleepFrame() override;

  // Offline is the normal resting state: do not spin or pin the device awake
  // merely because no network is available. Pull wakes stay awake until their
  // bounded sync attempt finishes.
  bool skipLoopDelay() override {
    return dashState != DashState::WifiSelection && dashState != DashState::Offline && dashState != DashState::Online;
  }
  bool preventAutoSleep() override {
    return pullMode || manualSyncActive || manualOtaIncrementalActive || manualOtaResumePending || pullOtaDownloading ||
           AgentDeck::OtaWs::receiving() || AgentDeck::OtaWs::flashPending();
  }

 private:
  // WifiJoining = background STA join with saved credentials (no picker UI —
  // the Face renders immediately and the join is just a status line). The
  // interactive WifiSelection picker is only pushed when there are no saved
  // credentials or the background join times out.
  // Online is Pocket's normal authenticated state: Wi-Fi is up and the cached
  // daemon endpoint is ready for bounded HTTP pulls. X3/X4 do not keep a live
  // WebSocket beside the HTTP client; doing both fragments the no-PSRAM X3
  // heap and provides no value to the offline-first Pocket surface.
  enum class DashState : uint8_t { Offline, WifiSelection, WifiJoining, Discovering, Connecting, Connected, Online };

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

  // Compact row for the local book or one daemon-authored Pocket item. Fixed
  // buffers keep the render-task stack bounded on the no-PSRAM C3.
  struct OverviewRow {
    char sid[72];  // session id or autonomous module cardId
    char project[40];
    char agentType[16];
    char controlMode[12];
    char state[20];
    char activity[AgentDeck::SESSION_ACTIVITY_CAP];
    bool awaiting;
    bool pocket;
    bool reading;
  };

  // Local reading + an always-available study card + up to the remaining
  // portable slots. The Pocket-reader feed currently authors at most two host
  // cards, so four bounded rows cover its full live contract without growing
  // both cross-task scratch arrays on the no-PSRAM X3.
  static constexpr int kOverviewCap = 1 + PocketDaily::CARD_CAP;
  // Scratch lives in the heap-allocated Activity object, not either task stack.
  // Each buffer is 4 bounded rows (~1.5 KB), allocated once with the activity
  // and reused; separate render/loop copies prevent cross-task races.
  OverviewRow renderRows[kOverviewCap] = {};
  OverviewRow inputRows[kOverviewCap] = {};
  // GlanceInfo is ~600 B. Reuse a render-task-only snapshot instead of adding
  // it to the nested render() -> renderGlance() stack chain.
  PocketDaily::Glance renderGlanceSnapshot{};
  // Absolute daemon-local time of the feed backing the snapshot. It is never
  // advanced, so a retained frame can honestly label it as the sync time.
  char renderSyncedHm[6] = {0};
  uint32_t renderSavedEpoch = 0;
  // The Daily Brief and retained sleep frame show at most one carried learning
  // item. Keep the ~500 B card and ~200 B book summary in the heap-allocated
  // activity object, never on the 12 KB render stack.
  PocketDaily::Card renderPocketSnapshot{};
  // Firmware-authored Japanese daily word. Unlike daemon cards this survives a
  // completely offline first boot and never writes a meaningless host outbox
  // decision. Guarded by the AgentDeck state mutex when copied across tasks.
  PocketDaily::Card localStudyCard{};
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

  // Which screen the Connected dashboard is showing. Overview is home; OK opens a
  // session's read-only Detail (timeline + inline options as a fallback grammar).
  // Card is the primary decision surface: it auto-surfaces from Overview when any
  // session needs attention, and maps the front buttons to the choices directly.
  enum class ViewMode : uint8_t { Overview, Detail, Card };

  void onWifiSelectionComplete(bool connected);
  void launchWifiPicker();
  bool startSavedWifiJoin(bool pickerOnFailure = false);
  bool beginSavedWifiConnection(const char* ssid);
  void startNetworking();
  void sendClientRegister();
  void sendDeviceInfo();
  uint32_t computeStateSignature() const;

  // Snapshot one selected awaiting session. Detail never needs the old triage
  // array, so this avoids placing six large question buffers on the C3 stack.
  bool findAwaiting(const char* sid, AwaitingItem& out) const;
  bool findPocketCard(const char* cardId, PocketDaily::Card& out) const;
  // Auto-surface / auto-resolve the Card view. Runs from loop() while Connected.
  void serviceCard();
  // True when the card can bind choices directly to front buttons (softkey
  // grammar); false falls back to the cursor grammar inside the card.
  static bool cardUsesSoftkeys(AgentDeck::AttentionMode mode, uint8_t optionCount);
  // Fill out[] with all alive sessions (overview order); returns the count.
  int collectOverview(OverviewRow* out, int cap) const;
  void handleButtons();
  bool applyDecision(const AwaitingItem& it, int optionCursor);
  bool applyPocketChoice(const PocketDaily::Card& card, int optionCursor);
  bool deferPocketCard(const PocketDaily::Card& card);
  void dismissPocketCard(const char* cardId);
  // fromCache renders the persisted deck (display-only, no selection/Open) with
  // an "as of" sync-age line derived from asOfEpoch. Live renders pass false/0.
  void renderOverview(const OverviewRow* rows, int n, int awaitingCount, bool fromCache, uint32_t asOfEpoch);
  void renderDetail();
  void renderCard();
  void renderPocketCard(const PocketDaily::Card& card);
  // Why the glance is being painted. It is not a sleep-only screen: the same
  // layout is the dashboard's ambient face whenever there are no live session
  // cards to show (offline, booting, or simply nothing running), which is what
  // makes the device useful without a daemon or a network.
  enum class GlanceReason : uint8_t {
    Ambient,     // live Face: no session cards — show information, not an apology
    TimedSleep,  // battery cadence: "next ~HH:MM"
    PoweredOff,  // user held power: no next sync to promise
  };
  // Weather + provider quota + work wrap-up from the feed's glance block. Times
  // are ABSOLUTE ("Synced HH:MM · next ~HH:MM") because a retained e-ink frame
  // must stay true without a repaint. Sleep paints use FULL_REFRESH every
  // kGlanceFullRefreshEvery-th frame to clear accumulated ghosting.
  void renderGlance(GlanceReason reason);
  // Branded header (AgentDeck mark + title) shared by every Connected screen.
  void drawBrandedHeader(const char* title, const char* subtitle) const;

  // The daemon port is dynamic (9120, falling back up to 9139), so a cached
  // ip:port goes stale across a daemon restart. If a connect attempt doesn't
  // succeed within this window, drop back to Discovering and re-resolve via mDNS
  // instead of hammering the old port. See feedback_daemon_port_flexibility.
  static constexpr uint32_t kConnectTimeoutMs = 10000;
  // Background STA join budget before falling back to the interactive picker.
  static constexpr uint32_t kWifiJoinTimeoutMs = 20000;
  // ── Radio bring-up heap floor ──
  // Raising the radio is the largest allocation burst this Face makes, and the
  // riskiest part is not ours: every driver event is copied into a fresh
  // arduino_event_t by NetworkEvents::postEvent() on the esp_event task. That
  // `new` is Arduino core code, and in a -fno-exceptions build a failed
  // allocation is abort(), not nullptr — so an event arriving on a starved heap
  // panics the whole device from a task we cannot guard from the inside.
  // Thresholds come from the device's own telemetry in /agentdeck.log: a
  // healthy Pocket sits at free 29-35 KB / largest 16-25 KB, while the decayed
  // state that preceded the abort logged free 6-12 KB / largest 0.9-2 KB. Gate
  // between the two observed bands.
  static constexpr uint32_t kWifiBringUpMinBlock = 4096;
  static constexpr uint32_t kWifiBringUpMinFree = 12288;
  // Latched by the gate above. Callers must not "helpfully" escalate a heap
  // refusal into the interactive picker: WifiSelectionActivity scans too, so
  // that would walk straight back into the abort we just declined. Also keeps
  // the refusal to one log line per episode on the 2 s OTA-resume retry.
  bool wifiHeapBlocked = false;

  // ── Sync outcome ──
  // A Sync press used to end silently: the status line went back to the idle
  // label whether the pull had updated the deck, found nothing new, or never
  // reached the Companion at all. Latch what actually happened and say so for
  // a few seconds, so the button has a visible answer.
  enum class SyncOutcome : uint8_t { None, Updated, UpToDate, Unreachable };
  SyncOutcome lastSyncOutcome = SyncOutcome::None;
  uint32_t lastSyncOutcomeMs = 0;
  static constexpr uint32_t kSyncOutcomeHoldMs = 20000;
  // Worst-case wall time of one Sync press, shown up front. The HTTP call
  // blocks the loop, so the Face cannot count down — but it can at least say
  // how long the freeze is bounded to instead of leaving the user guessing.
  static constexpr unsigned kSyncWorstCaseSec = 16;
  // ── Heap decay watch ──
  // The abort happened 7 minutes into a boot with no telemetry between the
  // successful sync at 12 s and the fatal scan at 429 s. Sample the heap on a
  // slow tick and log only new low-water marks, so the next occurrence carries
  // the trend that explains it without flooding the SD log.
  // Sampling backs off 2s -> 30s. A flat 30s missed the entire collapse: the
  // first sample read 43 KB free and the device was already under 6 KB by the
  // time the second would have fired. Bring-up needs the dense end.
  static constexpr uint32_t kHeapWatchFastMs = 2000;
  static constexpr uint32_t kHeapWatchSlowMs = 30000;
  static constexpr uint32_t kHeapWatchStepBytes = 2048;
  uint32_t heapWatchNextMs = 0;
  uint32_t heapWatchIntervalMs = kHeapWatchFastMs;
  uint32_t heapWatchLowFree = 0;
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
  char joiningSsid[33] = {0};        // SSID of the in-progress background join (status line)
  bool savedWifiJoinFailed = false;  // next explicit Sync opens the picker after a timed-out saved join
  bool savedWifiScanActive = false;  // async scan; Pocket remains navigable while it runs
  bool savedWifiPickerOnFailure = false;

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
  int overviewCursor = 0;      // selected Reading/Study item in the Home carousel
  int overviewTop = 0;         // first visible row (scroll window)
  int detailScroll = 0;        // first visible content line in Detail
  int detailMaxScroll = 0;     // set by renderDetail; lets handleButtons know "at bottom"
  char selectedSid[72] = {0};  // session id or autonomous module cardId
  // Returns a script-appropriate SD CJK font id when available, else the given
  // UI font. The shared selector validates real glyph coverage, which matters
  // for Japanese: the old dashboard-local Hangul font accepted Kana/Han lines
  // and then rendered them blank.
  int fontForText(int uiFontId, const char* text) const;

  // Decision-card / triage cursors
  int triageIndex = 0;       // which awaiting session is shown
  int optionCursor = 0;      // which option is highlighted (option prompts)
  uint32_t backPressMs = 0;  // Back press timestamp for short(deny)/long(exit/back)
  uint32_t lastDecisionMs = 0;

  // ── Card view state ──
  char cardSid[72] = {0};      // session or autonomous module card the Card is showing
  uint32_t cardSig = 0;        // signature of the prompt the Card is showing
  bool cardFocusSent = false;  // focus_session sent for this card (WaitingForOptions)
  // Card→Detail transitions consume a raw front-button PRESS; the mapped RELEASE
  // of that same press would otherwise land in Detail's Confirm handler and could
  // fire an inline decision (cooldown only covers short presses). Swallow it.
  bool swallowConfirmRelease = false;
  uint32_t lastUserInputMs = 0;  // any button press — suppresses auto-surface mid-navigation
  // Legacy live-card dismissal state. Live cards are no longer surfaced by
  // Pocket, but retaining the bounded fallback keeps stale view state safe.
  static constexpr int kDismissedCap = 8;
  uint32_t dismissedSigs[kDismissedCap] = {0};
  uint8_t dismissedHead = 0;

  // ── M5.5 Deck persistence ──
  // The persisted deck doubles as the offline fallback: loaded from SD in
  // onEnter (so boot renders the last-synced deck before any connection) and
  // refreshed in place whenever the live deck's content signature changes.
  // Guarded by g_stateMutex: written on the loop task, read on the render task.
  // Heap (unique_ptr), not a member array — ~3.5 KB must not sit on the C3 stack.
  std::unique_ptr<PocketDaily::DeckStore::Snapshot> cachedDeck;

  uint32_t lastDeckSig = 0;     // content signature of the last persisted deck
  uint32_t lastDeckSaveMs = 0;  // SD-write throttle
  bool clockSynced = false;     // SNTP landed — repaint once so "as of" gains an age
  static constexpr uint32_t kDeckSaveIntervalMs = 5000;

  // Persist the live deck to SD when its content changed (throttled; loop task).
  void serviceDeckPersist();
  // Current unix-seconds estimate: NTP clock first, daemon-clock estimate else 0.
  static uint32_t bestEpochNow();

  // ── M6 pull-sync power ladder ──
  // Timer wake with the battery cadence enabled: join Wi-Fi, sync once over
  // HTTP (GET /feed + outbox push), repaint the Face, then deep-sleep again
  // with the timer armed. Any button press cancels into the interactive WS
  // flow; USB power disables the cadence entirely (docked = live mode).
  bool pullMode = false;
  bool pullSynced = false;
  bool pullEndpointTried = false;  // cached-endpoint fast path attempted
  // Which glance variant render() should paint. Non-Ambient means this frame is
  // the one the panel keeps through sleep.
  GlanceReason glanceReason = GlanceReason::Ambient;
  // ── M9 stage 1: local plane (the open book) on the glance ──
  // True while the Ambient glance is the face on screen. Written by render()
  // (render task), read by handleButtons() (loop task) to give Confirm its
  // "resume reading" meaning only when the glance is actually displayed —
  // a stale read is benign (one inert or late press), so no lock.
  bool ambientGlanceShown = false;
  // Confirm on the glance face: onExit restarts into the reader
  // (silentRestartToReader) instead of Home.
  bool exitToReader = false;
  bool sleepFramePending = false;  // render() must paint the sleep glance
  uint32_t pullSyncedAtMs = 0;
  uint32_t pullNextSec = 0;  // daemon's nextPullSec hint (0 → default)
  // Conditional pull: deckSig of the last applied feed (seeded from the SD deck
  // cache at onEnter, refreshed on every full feed). Echoed as `?sig=` so an
  // unchanged deck costs one tiny response. Empty = always pull the full feed.
  char lastFeedSig[12] = {0};
  // Next-pull wall time (daemon-local "HH:MM"), precomputed in beginTimedSleep
  // because only it knows the sleep length; empty when no pull has anchored
  // wall time yet. The *synced* time is computed at paint, not stored.
  char sleepNextHm[6] = {0};
  uint32_t sleepForSec = 0;                                    // fallback "sleeping ~Nm" when no wall time known
  static constexpr uint32_t kGlanceFullRefreshEvery = 8;       // ghost-clearing cadence
  uint32_t enterMs = 0;                                        // onEnter millis — pull budget / idle anchor
  static constexpr uint32_t kPullLingerMs = 20000;             // user-presence window after a sync
  static constexpr uint32_t kPullBudgetMs = 60000;             // whole wake budget before sleeping unsynced
  static constexpr uint32_t kPullDefaultSec = 3600;            // no daemon hint → hourly
  static constexpr uint32_t kPullActiveSec = 900;              // sessions mid-turn → 15 min cadence
  static constexpr uint32_t kPullMinSec = 300;                 // clamp against a degenerate hint
  static constexpr uint32_t kIdleToCadenceMs = 5 * 60 * 1000;  // interactive → cadence handoff
  // One HTTP sync against the given endpoint; updates the pull state on success.
  bool attemptPullSync(const AgentDeck::Net::EndpointCandidates& endpoints, const char* token);
  // The WS live socket carries sessions/usage but NOT the glance block
  // (weather / daemon-authored wrap-up) — that only rides `GET /feed`. Pull it
  // over HTTP when the current glance is older than maxAgeMs, using the live
  // WS endpoint when connected, else the cached one. Blocking (~1-2s), and the
  // chain underneath (esp_http_client → std::string body → ArduinoJson →
  // deck persist → SD) runs DEEP — call it only from a shallow stack: the top
  // of loop() (via glanceRefreshQueued) or the sleep-frame paint path. Calling
  // it inline at the WS-connect edge overflowed the loop task stack (panic at
  // the stack-bottom canary right after device_info, 3/3 reproducible).
  // Returns true when a fresh (changed) feed was applied.
  bool refreshGlanceIfStale(uint32_t maxAgeMs);
  // Named heap sample. Bring-up is where this device actually dies: free heap
  // measured 43 KB before the radio and could not satisfy 6.2 KB eight seconds
  // later, with nothing logged in between. One line per step turns that black
  // box into a ledger. Cheap enough to leave in — bring-up runs once per wake.
  void logHeapStage(const char* stage) const;
  // Bring the mDNS responder/browser and the UDP beacon listener up, once.
  // Measured cost on X3: 5,596 B (mDNS) + 1,964 B (UDP) of a heap that has
  // only ~26 KB left once the radio is up — so these are paid for only when
  // discovery is actually needed. A paired device syncing against its cached
  // endpoint never needs either; both failover paths route through
  // DashState::Discovering, which calls this before the first poll.
  void ensureDiscoveryServices();
  bool discoveryServicesUp = false;
  // Dedicated Home Sync action. Unlike the background cadence it runs while
  // Pocket Daily is awake and checks a staged pull-OTA advert even when the
  // live WebSocket is unavailable.
  bool attemptManualSync();
  // Set at the Connected transition; the fetch itself runs from the top of
  // loop() on the next pass, where the call stack is shallowest.
  bool glanceRefreshQueued = false;
  // A user pressing the dedicated Home Sync key bypasses the normal 30-minute
  // freshness guard. The actual HTTP work still runs from the shallow top of
  // loop(), never from the input stack.
  bool forceGlanceRefresh = false;
  bool manualSyncQueued = false;
  bool manualSyncActive = false;
  // Pull OTA is resumable on SD, but a bounded tryInstall() pass can end after
  // a handful of segments. Keep retry ownership in the activity so one Sync
  // continues until validation/flash, including Wi-Fi/server reconnection.
  bool manualOtaResumePending = false;
  // Awake OTA runs as bounded SD/HTTP chunks. The input loop executes between
  // chunks, so carousel/read/study controls remain usable during download.
  bool manualOtaIncrementalActive = false;
  bool pullOtaDownloading = false;
  int8_t pullOtaPctBucket = -1;
  uint8_t manualOtaNoProgressRetries = 0;
  uint32_t manualOtaResumeAtMs = 0;
  uint32_t manualOtaResumeStartedMs = 0;
  uint32_t pullOtaDownloadedBytes = 0;
  uint32_t pullOtaTotalBytes = 0;
  char lastManualOtaMd5[33] = {0};
  static constexpr uint32_t kManualOtaResumeWindowMs = 30 * 60 * 1000;
  // A failed bounded HTTP pull stays on the cached Pocket instead of entering
  // an automatic discovery→pull loop. The next explicit Sync performs one
  // fresh discovery, then exactly one retry.
  bool manualSyncNeedsDiscovery = false;
  // True only for the one bounded discovery retry that follows a cached-set
  // failure. Prevents discovery→pull loops while making first-boot ADE1
  // migration automatic instead of requiring a second button press.
  bool manualSyncDiscoveryRetryActive = false;
  // Pull-mode step, run from loop(): cached-endpoint fast path + sleep decisions.
  void servicePullSync();
  // Interactive-mode idle → timed sleep handoff (cadence setting on, on battery).
  void serviceIdleCadence();
  // Final deck persist + honest "sleeping" paint, then timed deep sleep.
  void beginTimedSleep(uint32_t seconds);
};
