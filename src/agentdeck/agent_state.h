#pragma once
//
// agent_state.h — TRIMMED port of AgentDeck esp32/src/state/agent_state.h.
//
// Keeps only the networking + agent fields the M2 (display-only) layer needs:
//   • AgentState enum
//   • PromptOption + SessionInfo
//   • a minimal DashboardState (connection / agent / usage / prompt / sessions)
//
// DROPPED from the original (LVGL/board/terrarium concerns, not needed here):
//   • CreatureState / CrayfishState / TetraState enums + derivation
//   • Gateway (OpenClaw) topology fields
//   • TimelineEntry ring buffer
//   • host display_state / dim / orientation fields
//   • HUD view-state flags
//
// g_state is read on the render task and written on the main (loop) task, so it
// is still guarded by g_stateMutex exactly like the original.
//
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstdint>
#include <cstring>

#include "agentdeck_config.h"
#include "glance_state.h"
#include "pocket_daily/models.h"

namespace AgentDeck {

// Rich enough for a 2–3 line E-ink card summary while remaining strictly
// bounded on the no-PSRAM X3/X4. Ten sessions consume 1,920 bytes total.
static constexpr size_t SESSION_ACTIVITY_CAP = 192;

// ===== Agent state enum =====
// Wire strings: disconnected|idle|processing|awaiting_permission|awaiting_option|awaiting_diff
enum class AgentState : uint8_t {
  DISCONNECTED = 0,
  IDLE,
  PROCESSING,
  AWAITING_PERMISSION,
  AWAITING_OPTION,
  AWAITING_DIFF
};

// ===== Prompt option =====
struct PromptOption {
  char label[80];
  char action[40];
  uint8_t index;
  bool recommended;
  bool selected;
};

// Compatibility aliases for the existing AgentDeck protocol adapter. Product
// code owns these fixed-size models in PocketDaily; the provider only fills them.
using PocketChoice = PocketDaily::Choice;
using PocketCard = PocketDaily::Card;
static constexpr uint8_t POCKET_CARD_CAP = PocketDaily::CARD_CAP;

// ===== Session info (multi-agent) =====
struct SessionInfo {
  // Daemon session ids are UUIDs (36 ch) and can be prefixed ("observed:claude:<uuid>"
  // ≈ 52 ch). The WebSocket path does NOT truncate ids (unlike the serial path), so
  // size to hold the longest form — a short buffer mis-routes approve/deny.
  char id[64];
  char projectName[40];
  char modelName[32];
  char agentType[16];    // "claude-code" / "openclaw" / "codex-cli" / "codex-app" / "opencode"
  char controlMode[12];  // "managed" / "observed"; required for honest remote actions
  char state[20];
  uint16_t port;
  bool alive;
  // Per-session detail (tool/elapsed + awaiting prompt). Populated from the
  // enriched sessions_list; M3 uses requestId to drive approve/deny.
  char currentTool[40];
  uint32_t elapsedSec;
  char question[160];
  char promptType[20];
  char requestId[40];                   // gated PreToolUse request id → reply permission_decision (M3)
  char activity[SESSION_ACTIVITY_CAP];  // bounded current task + goal / latest-event summary
};

// One timeline event for the per-session Detail view. Populated from the daemon's
// `timeline_event` broadcast (live, forward-only). Bounded ring on a no-PSRAM C3.
struct TimelineItem {
  char sid[64];    // entry.sessionId ("" = unattributed error/scheduled row)
  char text[96];   // entry.raw (human line)
  char type[20];   // entry.type (chat_start / tool_request / …)
  uint32_t tsSec;  // entry.ts / 1000 (daemon epoch seconds); 0 = unknown
};

// ===== Main dashboard state (trimmed) =====
struct DashboardState {
  // Connection
  bool wsConnected;
  char bridgeIp[16];
  uint16_t bridgePort;
  char authToken[40];
  uint32_t lastMessageMs;  // millis() of last JSON received; 0 = never

  // Agent (from state_update)
  AgentState state;
  char projectName[40];
  char modelName[32];
  char agentType[16];
  char effortLevel[8];
  char sessionId[64];  // focused session id (M3 routing) — full/prefixed UUID
  char focusedSessionId[64];
  char requestId[40];  // gated request id for the focused awaiting prompt (M3)
  bool navigable;
  int cursorIndex;

  // Current tool (processing indicator)
  char currentTool[40];
  char toolInput[80];

  // Permission / Options
  char question[200];
  char promptType[20];
  PromptOption options[8];
  uint8_t optionCount;
  char optionSessionId[64];  // sessionId whose state_update/prompt_options owns options[]

  // Usage (from usage_update)
  float fiveHourPercent;   // 0-100, -1 = no data
  float sevenDayPercent;   // 0-100, -1 = no data
  char fiveHourReset[32];  // ISO-8601 resetsAt (verbatim; firmware formats remaining)
  char sevenDayReset[32];
  uint32_t inputTokens;
  uint32_t outputTokens;
  uint32_t toolCalls;
  uint32_t sessionDurationSec;
  float estimatedCostUsd;
  bool usageStale;

  // Other-agent subscription / limit summary (best-effort; only present when the
  // hub supplies it). Empty string / -1 = no data.
  char codexPlan[16];         // ChatGPT/Codex plan ("plus", "pro", …)
  char codexActiveUntil[32];  // ISO date the ChatGPT subscription is active until
  char antigravityPlan[24];   // Antigravity plan name
  float antigravityCredits;   // Antigravity available credits, -1 = no data
  // Codex rate-limit windows (usage_update.codexRateLimits). primary ≈ 5h,
  // secondary ≈ 7d. -1 = no data.
  float codexFivePercent;
  float codexSevenPercent;
  char codexFiveReset[32];
  char codexSevenReset[32];

  // Per-session timeline ring (forward-only, from timeline_event). Detail view.
  static constexpr int TIMELINE_CAP = 16;
  TimelineItem timeline[TIMELINE_CAP];
  uint8_t timelineCount;      // number of valid entries (<= TIMELINE_CAP)
  uint8_t timelineHead;       // next write index (oldest = head when full)
  uint32_t timelineRevision;  // increments on event append and queried-history replacement

  // Daemon wall-clock estimate (the C3 has no RTC/NTP). Newest entry.ts seen
  // and the local millis() at which it arrived; render derives per-entry age as
  // (daemonEpochSec + elapsed-since-arrival) - entry.tsSec. 0 = no sample yet.
  uint32_t daemonEpochSec;
  uint32_t daemonEpochAtMs;

  // Daemon-local wall time ("HH:MM") from the last card-feed pull + the local
  // millis() it arrived. The device has no timezone, so daemon-local HH:MM is
  // the ONLY honest wall-clock face it can show — the sleep glance derives
  // "Synced HH:MM · next ~HH:MM" from this. Empty = no pull yet this boot.
  char serverHm[6];
  uint32_t serverHmAtMs;

  // Sleep-glance content from the card feed (weather / provider quota /
  // work wrap-up). Survives markBridgeDisconnected() like the deck cache does:
  // it is a snapshot with its own staleness semantics, not live state.
  GlanceInfo glance;
  // millis() when `glance` was last applied from a feed; 0 = never this boot.
  // Drives the WS-mode staleness check — the live socket carries sessions and
  // usage but NOT the glance block, so weather only stays fresh by pulling
  // /feed occasionally even while connected.
  uint32_t glanceAtMs;

  // Sessions (multi-agent). Cap matches AgentDeckCfg::SESSIONS_CAP.
  SessionInfo sessions[AgentDeckCfg::SESSIONS_CAP];
  uint8_t sessionCount;

  // Autonomous daemon content. Survives bridge disconnect like the glance:
  // each card is a timestamped feed snapshot and day-class choices queue via
  // the SD outbox instead of pretending the live socket still owns it.
  PocketCard pocketCards[POCKET_CARD_CAP];
  uint8_t pocketCount;

  // Data reception tracking
  bool dataReceived;  // true after first state_update / sessions_list

  void reset() {
    memset(this, 0, sizeof(DashboardState));
    state = AgentState::DISCONNECTED;
    navigable = false;
    cursorIndex = 0;
    // Sentinel -1.0f = "no data" (0 is a valid usage value)
    fiveHourPercent = -1.0f;
    sevenDayPercent = -1.0f;
    estimatedCostUsd = -1.0f;
    antigravityCredits = -1.0f;
    codexFivePercent = -1.0f;
    codexSevenPercent = -1.0f;
    glance.clear();  // restores the non-zero "no data" sentinels
  }

  // Called while g_stateMutex is held. Clears volatile bridge data so every
  // surface renders a disconnected state instead of stale session data.
  void markBridgeDisconnected() {
    wsConnected = false;
    state = AgentState::DISCONNECTED;
    projectName[0] = '\0';
    modelName[0] = '\0';
    agentType[0] = '\0';
    effortLevel[0] = '\0';
    sessionId[0] = '\0';
    focusedSessionId[0] = '\0';
    requestId[0] = '\0';
    navigable = false;
    cursorIndex = 0;
    currentTool[0] = '\0';
    toolInput[0] = '\0';
    question[0] = '\0';
    promptType[0] = '\0';
    optionCount = 0;
    optionSessionId[0] = '\0';
    sessionCount = 0;
    fiveHourPercent = -1.0f;
    sevenDayPercent = -1.0f;
    fiveHourReset[0] = '\0';
    sevenDayReset[0] = '\0';
    codexPlan[0] = '\0';
    codexActiveUntil[0] = '\0';
    antigravityPlan[0] = '\0';
    antigravityCredits = -1.0f;
    codexFivePercent = -1.0f;
    codexSevenPercent = -1.0f;
    codexFiveReset[0] = '\0';
    codexSevenReset[0] = '\0';
    timelineCount = 0;
    timelineHead = 0;
    timelineRevision = 0;
    daemonEpochSec = 0;
    daemonEpochAtMs = 0;
    usageStale = true;
    dataReceived = false;
  }
};

// Global state — accessed from main (loop) + render task (use mutex)
extern DashboardState g_state;
extern SemaphoreHandle_t g_stateMutex;

// Lazily create the mutex on first use so call order never matters.
inline void ensureStateMutex() {
  if (!g_stateMutex) g_stateMutex = xSemaphoreCreateMutex();
}
inline void lockState() {
  if (g_stateMutex) xSemaphoreTake(g_stateMutex, portMAX_DELAY);
}
inline void unlockState() {
  if (g_stateMutex) xSemaphoreGive(g_stateMutex);
}

}  // namespace AgentDeck
