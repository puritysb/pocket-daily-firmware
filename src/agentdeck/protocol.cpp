#include "protocol.h"

#include <Arduino.h>
#include <ArduinoJson.h>

#include <algorithm>

#include "agent/AgentLog.h"
#include "agent_state.h"
#include "agentdeck_config.h"

namespace AgentDeck {

// ── Global state definitions (declared extern in agent_state.h) ──
// Written on the main/loop task (parseMessage), read on the render task.
DashboardState g_state;
SemaphoreHandle_t g_stateMutex = nullptr;

namespace {

AgentState parseState(const char* s) {
  if (!s) return AgentState::DISCONNECTED;
  if (strcmp(s, "idle") == 0) return AgentState::IDLE;
  if (strcmp(s, "processing") == 0) return AgentState::PROCESSING;
  if (strcmp(s, "awaiting_permission") == 0) return AgentState::AWAITING_PERMISSION;
  if (strcmp(s, "awaiting_option") == 0) return AgentState::AWAITING_OPTION;
  if (strcmp(s, "awaiting_diff") == 0) return AgentState::AWAITING_DIFF;
  return AgentState::DISCONNECTED;
}

// Reusable JSON documents — sized elastically by ArduinoJson 7.
JsonDocument doc;
JsonDocument filter;

void copyStr(char* dst, size_t cap, const char* src) {
  if (!src) {
    dst[0] = '\0';
    return;
  }
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = '\0';
}

// Append a distinct summary fragment without ever growing a heap string. The
// sessions payload may provide a terse `activity` plus a richer `goal`; keeping
// both gives the card enough material for its 2–3 line layout.
void appendSummary(char* dst, size_t cap, const char* text) {
  if (!dst || cap == 0 || !text || !text[0] || strcmp(dst, text) == 0) return;
  const size_t used = strlen(dst);
  if (used >= cap - 1) return;
  snprintf(dst + used, cap - used, "%s%s", used ? " - " : "", text);
}

// Copy a prompt option snapshot into the fixed DashboardState arrays. Caller
// holds g_stateMutex. `ownerSid` is mandatory for actions: source-less options
// may be displayed nowhere, preventing a late previous-focus event from
// becoming buttons for the newly selected session.
void storeOptionsLocked(JsonObject& obj, const char* ownerSid, bool clearWhenMissing) {
  if (!obj["options"].is<JsonArray>()) {
    if (clearWhenMissing) {
      g_state.optionCount = 0;
      g_state.optionSessionId[0] = '\0';
    }
    return;
  }
  JsonArray opts = obj["options"].as<JsonArray>();
  g_state.optionCount = static_cast<uint8_t>(std::min(static_cast<int>(opts.size()), 8));
  for (uint8_t i = 0; i < g_state.optionCount; i++) {
    JsonObject o = opts[i].as<JsonObject>();
    copyStr(g_state.options[i].label, sizeof(g_state.options[i].label), o["label"] | "");
    copyStr(g_state.options[i].action, sizeof(g_state.options[i].action), o["shortcut"] | "");
    g_state.options[i].index = o["index"] | i;
    g_state.options[i].recommended = o["recommended"] | false;
    g_state.options[i].selected = o["selected"] | false;
  }
  copyStr(g_state.optionSessionId, sizeof(g_state.optionSessionId), ownerSid ? ownerSid : "");
}

void handleStateUpdate(JsonObject& obj) {
  lockState();

  g_state.state = parseState(obj["state"].as<const char*>());

  // state_update is a full snapshot: write every field unconditionally (the
  // `| ""` default clears it when the key is absent) so a previous prompt's
  // sessionId / promptType / etc. can never carry over into the next one.
  copyStr(g_state.projectName, sizeof(g_state.projectName), obj["projectName"] | "");
  copyStr(g_state.modelName, sizeof(g_state.modelName), obj["modelName"] | "");
  copyStr(g_state.agentType, sizeof(g_state.agentType), obj["agentType"] | "");
  copyStr(g_state.effortLevel, sizeof(g_state.effortLevel), obj["effortLevel"] | "");

  // Session routing ids (M3 uses these for approve/deny targeting).
  copyStr(g_state.sessionId, sizeof(g_state.sessionId), obj["sessionId"] | "");
  copyStr(g_state.focusedSessionId, sizeof(g_state.focusedSessionId), obj["focusedSessionId"] | "");
  if (obj["requestId"].is<const char*>())
    copyStr(g_state.requestId, sizeof(g_state.requestId), obj["requestId"].as<const char*>());
  else
    g_state.requestId[0] = '\0';
  g_state.navigable = obj["navigable"] | false;
  g_state.cursorIndex = obj["cursorIndex"] | 0;

  // Current tool
  if (obj["currentTool"].is<const char*>())
    copyStr(g_state.currentTool, sizeof(g_state.currentTool), obj["currentTool"].as<const char*>());
  else
    g_state.currentTool[0] = '\0';
  if (obj["toolInput"].is<const char*>())
    copyStr(g_state.toolInput, sizeof(g_state.toolInput), obj["toolInput"].as<const char*>());
  else
    g_state.toolInput[0] = '\0';

  // Permission / options
  if (obj["question"].is<const char*>())
    copyStr(g_state.question, sizeof(g_state.question), obj["question"].as<const char*>());
  else
    g_state.question[0] = '\0';
  copyStr(g_state.promptType, sizeof(g_state.promptType), obj["promptType"] | "");

  // Only sessionId owns an option snapshot. focusedSessionId on an aggregate
  // daemon/OpenClaw state can name an observed row while carrying unrelated
  // aggregate options, so it must never be used as the owner.
  storeOptionsLocked(obj, obj["sessionId"] | "", true);

  g_state.dataReceived = true;
  unlockState();
}

void handlePromptOptions(JsonObject& obj) {
  const char* sid = obj["sessionId"] | (obj["focusedSessionId"] | "");
  lockState();
  if (obj["question"].is<const char*>())
    copyStr(g_state.question, sizeof(g_state.question), obj["question"].as<const char*>());
  if (obj["promptType"].is<const char*>())
    copyStr(g_state.promptType, sizeof(g_state.promptType), obj["promptType"].as<const char*>());
  storeOptionsLocked(obj, sid, false);
  g_state.dataReceived = true;
  unlockState();
}

void handleUsageUpdate(JsonObject& obj) {
  lockState();
  g_state.dataReceived = true;

  // -1.0f sentinel for "no data" (0 is a valid value).
  g_state.fiveHourPercent = obj["fiveHourPercent"].is<float>() ? obj["fiveHourPercent"].as<float>() : -1.0f;
  g_state.sevenDayPercent = obj["sevenDayPercent"].is<float>() ? obj["sevenDayPercent"].as<float>() : -1.0f;

  g_state.inputTokens = obj["inputTokens"] | g_state.inputTokens;
  g_state.outputTokens = obj["outputTokens"] | g_state.outputTokens;
  g_state.toolCalls = obj["toolCalls"] | g_state.toolCalls;
  g_state.sessionDurationSec = obj["sessionDurationSec"] | g_state.sessionDurationSec;
  g_state.estimatedCostUsd = obj["estimatedCostUsd"].is<float>() ? obj["estimatedCostUsd"].as<float>() : -1.0f;
  g_state.usageStale = obj["usageStale"] | false;

  // Reset times: only the already-formatted "Xh Ym" form is kept. The original
  // also parsed ISO-8601 + computed relative time via NTP; that path is dropped
  // for M2 (no NTP brought up here). ISO values are stored verbatim so a future
  // milestone can format them.
  auto storeReset = [&](const char* key, char* out, size_t cap) {
    if (obj[key].is<const char*>())
      copyStr(out, cap, obj[key].as<const char*>());
    else
      out[0] = '\0';
  };
  storeReset("fiveHourResetsAt", g_state.fiveHourReset, sizeof(g_state.fiveHourReset));
  storeReset("sevenDayResetsAt", g_state.sevenDayReset, sizeof(g_state.sevenDayReset));

  // Other-agent subscription/limit summary (best-effort — only some hubs send it).
  storeReset("codexPlanType", g_state.codexPlan, sizeof(g_state.codexPlan));
  storeReset("codexSubscriptionActiveUntil", g_state.codexActiveUntil, sizeof(g_state.codexActiveUntil));
  g_state.antigravityPlan[0] = '\0';
  g_state.antigravityCredits = -1.0f;
  if (obj["antigravityStatus"].is<JsonObject>()) {
    JsonObject ag = obj["antigravityStatus"].as<JsonObject>();
    if (ag["planName"].is<const char*>())
      copyStr(g_state.antigravityPlan, sizeof(g_state.antigravityPlan), ag["planName"].as<const char*>());
    // availableCredits arrives as an integer credit count.
    if (ag["availableCredits"].is<int>() || ag["availableCredits"].is<float>())
      g_state.antigravityCredits = ag["availableCredits"].as<float>();
  }

  // Codex rate-limit windows: primary ≈ 5h, secondary ≈ 7d (usedPercent + resetsAt).
  g_state.codexFivePercent = -1.0f;
  g_state.codexSevenPercent = -1.0f;
  g_state.codexFiveReset[0] = '\0';
  g_state.codexSevenReset[0] = '\0';
  if (obj["codexRateLimits"].is<JsonObject>()) {
    JsonObject cx = obj["codexRateLimits"].as<JsonObject>();
    auto window = [&](const char* key, float& pct, char* reset, size_t cap) {
      if (!cx[key].is<JsonObject>()) return;
      JsonObject wnd = cx[key].as<JsonObject>();
      if (wnd["usedPercent"].is<int>() || wnd["usedPercent"].is<float>()) pct = wnd["usedPercent"].as<float>();
      if (wnd["resetsAt"].is<const char*>()) copyStr(reset, cap, wnd["resetsAt"].as<const char*>());
    };
    window("primary", g_state.codexFivePercent, g_state.codexFiveReset, sizeof(g_state.codexFiveReset));
    window("secondary", g_state.codexSevenPercent, g_state.codexSevenReset, sizeof(g_state.codexSevenReset));
    // If codexPlanType wasn't sent at top-level, the windows object may carry it.
    if (g_state.codexPlan[0] == '\0' && cx["planType"].is<const char*>())
      copyStr(g_state.codexPlan, sizeof(g_state.codexPlan), cx["planType"].as<const char*>());
  }

  unlockState();
}

// Land one wire session object into a SessionInfo slot. Shared by the WS
// sessions_list handler and the M6 card_feed pull path so both transports
// produce byte-identical state. Caller holds g_stateMutex.
void parseSessionLocked(JsonObject& s, SessionInfo& si) {
  copyStr(si.id, sizeof(si.id), s["id"] | "");
  copyStr(si.projectName, sizeof(si.projectName), s["projectName"] | "");
  copyStr(si.modelName, sizeof(si.modelName), s["modelName"] | "");
  copyStr(si.agentType, sizeof(si.agentType), s["agentType"] | "");
  copyStr(si.controlMode, sizeof(si.controlMode), s["controlMode"] | "");
  copyStr(si.state, sizeof(si.state), s["state"] | "");
  si.port = s["port"] | 0;
  si.alive = s["alive"] | false;
  copyStr(si.currentTool, sizeof(si.currentTool), s["currentTool"] | "");
  si.elapsedSec = s["elapsedSec"] | 0;
  copyStr(si.question, sizeof(si.question), s["question"] | "");
  copyStr(si.promptType, sizeof(si.promptType), s["promptType"] | "");
  copyStr(si.requestId, sizeof(si.requestId), s["requestId"] | "");
  // Prefer the daemon's concise current action, then retain the richer goal
  // (or latest event) so Overview and Detail can use the available 2–3 lines.
  // All composition stays inside SessionInfo's fixed 192-byte buffer.
  si.activity[0] = '\0';
  const char* activity = s["activity"] | "";
  const char* currentTask = s["currentTask"] | "";
  const char* goal = s["goal"] | "";
  const char* lastEvent = s["lastEventText"] | "";
  appendSummary(si.activity, sizeof(si.activity), activity[0] ? activity : currentTask);
  appendSummary(si.activity, sizeof(si.activity), goal);
  if (si.activity[0] == '\0') appendSummary(si.activity, sizeof(si.activity), lastEvent);
  if (si.activity[0] == '\0') appendSummary(si.activity, sizeof(si.activity), si.currentTool);
}

void handleSessionsList(JsonObject& obj) {
  lockState();
  g_state.dataReceived = true;

  JsonArray sessions = obj["sessions"].as<JsonArray>();
  g_state.sessionCount = (uint8_t)std::min((int)sessions.size(), AgentDeckCfg::SESSIONS_CAP);

  for (uint8_t i = 0; i < g_state.sessionCount; i++) {
    JsonObject s = sessions[i].as<JsonObject>();
    parseSessionLocked(s, g_state.sessions[i]);
  }

  unlockState();
}

// Unattributed rows (no sessionId) are kept only when they carry a global
// signal every surface shows: errors and scheduled work. Everything else
// belongs to a session detail and is meaningless without one.
bool keepUnattributedTimelineType(const char* etype) {
  return strcmp(etype, "error") == 0 || strcmp(etype, "scheduled") == 0;
}

// Append one entry into the bounded ring + refresh the daemon-clock estimate.
// Caller holds g_stateMutex.
void appendTimelineLocked(const char* sid, const char* raw, const char* etype, uint32_t tsSec) {
  TimelineItem& it = g_state.timeline[g_state.timelineHead];
  copyStr(it.sid, sizeof(it.sid), sid);
  copyStr(it.text, sizeof(it.text), raw);
  copyStr(it.type, sizeof(it.type), etype);
  it.tsSec = tsSec;
  g_state.timelineHead = (g_state.timelineHead + 1) % DashboardState::TIMELINE_CAP;
  if (g_state.timelineCount < DashboardState::TIMELINE_CAP) g_state.timelineCount++;
  g_state.timelineRevision++;
  // The device has no RTC/NTP: track the newest daemon timestamp and when it
  // arrived so the render task can derive per-entry ages.
  if (tsSec > g_state.daemonEpochSec) {
    g_state.daemonEpochSec = tsSec;
    g_state.daemonEpochAtMs = millis();
  }
}

// Live, forward-only timeline ring (per-session Detail view). Appends entry.raw
// keyed by sessionId; the ring overwrites oldest when full.
void handleTimelineEvent(JsonObject& obj) {
  JsonObject e = obj["entry"].as<JsonObject>();
  if (e.isNull()) return;
  const char* sid = e["sessionId"] | "";
  const char* raw = e["raw"] | "";
  const char* etype = e["type"] | "";
  if (raw[0] == '\0' && etype[0] == '\0') return;
  if (sid[0] == '\0' && !keepUnattributedTimelineType(etype)) return;
  const uint32_t tsSec = static_cast<uint32_t>((e["ts"] | (uint64_t)0) / 1000ULL);

  lockState();
  appendTimelineLocked(sid, raw, etype, tsSec);
  unlockState();
}

// Reply to query_session_timeline: a batch of recent entries for one session
// (oldest→newest). Loads them into the ring so Detail fills on connect; the ring
// is bounded so the most recent TIMELINE_CAP entries win.
void handleTimelineHistory(JsonObject& obj) {
  JsonArray entries = obj["entries"].as<JsonArray>();
  if (entries.isNull()) return;
  lockState();
  // A query_session_timeline reply carries the requested sessionId. Replace the
  // mixed live ring with that authoritative history so entries from other busy
  // sessions cannot evict the selected session before Detail renders.
  const char* requestedSid = obj["sessionId"] | "";
  if (requestedSid[0]) {
    g_state.timelineCount = 0;
    g_state.timelineHead = 0;
    g_state.timelineRevision++;
  }
  for (JsonObject e : entries) {
    const char* sid = e["sessionId"] | "";
    const char* raw = e["raw"] | "";
    const char* etype = e["type"] | "";
    if (raw[0] == '\0' && etype[0] == '\0') continue;
    if (sid[0] == '\0' && !keepUnattributedTimelineType(etype)) continue;
    const uint32_t tsSec = static_cast<uint32_t>((e["ts"] | (uint64_t)0) / 1000ULL);
    appendTimelineLocked(sid, raw, etype, tsSec);
  }
  unlockState();
}

void configureJsonFilter() {
  filter.clear();

  filter["type"] = true;

  // state_update
  filter["state"] = true;
  filter["projectName"] = true;
  filter["modelName"] = true;
  filter["agentType"] = true;
  filter["effortLevel"] = true;
  filter["sessionId"] = true;
  filter["focusedSessionId"] = true;
  filter["requestId"] = true;
  filter["navigable"] = true;
  filter["cursorIndex"] = true;
  filter["currentTool"] = true;
  filter["toolInput"] = true;
  filter["question"] = true;
  filter["promptType"] = true;
  filter["options"][0]["label"] = true;
  filter["options"][0]["index"] = true;
  filter["options"][0]["recommended"] = true;
  filter["options"][0]["selected"] = true;
  filter["options"][0]["shortcut"] = true;

  // usage_update
  filter["fiveHourPercent"] = true;
  filter["sevenDayPercent"] = true;
  filter["inputTokens"] = true;
  filter["outputTokens"] = true;
  filter["toolCalls"] = true;
  filter["sessionDurationSec"] = true;
  filter["estimatedCostUsd"] = true;
  filter["usageStale"] = true;
  filter["fiveHourResetsAt"] = true;
  filter["sevenDayResetsAt"] = true;
  filter["codexPlanType"] = true;
  filter["codexSubscriptionActiveUntil"] = true;
  filter["antigravityStatus"]["planName"] = true;
  filter["antigravityStatus"]["availableCredits"] = true;
  filter["codexRateLimits"]["primary"]["usedPercent"] = true;
  filter["codexRateLimits"]["primary"]["resetsAt"] = true;
  filter["codexRateLimits"]["secondary"]["usedPercent"] = true;
  filter["codexRateLimits"]["secondary"]["resetsAt"] = true;
  filter["codexRateLimits"]["planType"] = true;

  // sessions_list
  filter["sessions"][0]["id"] = true;
  filter["sessions"][0]["projectName"] = true;
  filter["sessions"][0]["modelName"] = true;
  filter["sessions"][0]["agentType"] = true;
  filter["sessions"][0]["controlMode"] = true;
  filter["sessions"][0]["state"] = true;
  filter["sessions"][0]["port"] = true;
  filter["sessions"][0]["alive"] = true;
  filter["sessions"][0]["currentTool"] = true;
  filter["sessions"][0]["elapsedSec"] = true;
  filter["sessions"][0]["question"] = true;
  filter["sessions"][0]["promptType"] = true;
  filter["sessions"][0]["requestId"] = true;
  filter["sessions"][0]["activity"] = true;
  filter["sessions"][0]["currentTask"] = true;
  filter["sessions"][0]["goal"] = true;
  filter["sessions"][0]["lastEventText"] = true;

  // timeline_event / timeline_history
  filter["entry"]["sessionId"] = true;
  filter["entry"]["raw"] = true;
  filter["entry"]["type"] = true;
  filter["entry"]["ts"] = true;
  filter["entries"][0]["sessionId"] = true;
  filter["entries"][0]["raw"] = true;
  filter["entries"][0]["type"] = true;
  filter["entries"][0]["ts"] = true;
}

}  // namespace

namespace Protocol {

void parseMessage(const char* json, size_t length) {
  // Reject oversized frames before feeding the elastic JsonDocument — an
  // unbounded sessions_list would otherwise grow the doc until it
  // fragments/exhausts the heap on this no-PSRAM C3.
  if (length > AgentDeckCfg::PROTOCOL_MAX_MSG_BYTES) {
    AgentLog::line("PROTO", "frame too large: %u bytes (max %u) — dropped", (unsigned)length,
                   (unsigned)AgentDeckCfg::PROTOCOL_MAX_MSG_BYTES);
    return;
  }

  configureJsonFilter();
  doc.clear();
  DeserializationError err = deserializeJson(doc, json, length, DeserializationOption::Filter(filter));
  if (err) {
    AgentLog::line("PROTO", "JSON error: %s", err.c_str());
    return;
  }

  JsonObject obj = doc.as<JsonObject>();
  const char* type = obj["type"] | "";

  if (strcmp(type, "state_update") == 0) {
    handleStateUpdate(obj);
  } else if (strcmp(type, "prompt_options") == 0) {
    handlePromptOptions(obj);
  } else if (strcmp(type, "sessions_list") == 0) {
    handleSessionsList(obj);
  } else if (strcmp(type, "usage_update") == 0) {
    handleUsageUpdate(obj);
  } else if (strcmp(type, "timeline_event") == 0) {
    handleTimelineEvent(obj);
  } else if (strcmp(type, "timeline_history") == 0) {
    handleTimelineHistory(obj);
  } else if (strcmp(type, "connection") == 0 || strcmp(type, "connected") == 0) {
    // Connection ack — actual connect/disconnect is tracked by the WS event
    // callbacks. Logged for diagnostics.
    AgentLog::line("PROTO", "connection ack: %s", type);
  } else {
    // Accepted-and-ignored for M2. The original firmware also handled:
    //   device_info_request, display_state, set_orientation, wifi_provision,
    //   timeline_event, timeline_history, touch_diag
    // Those are board/host concerns not needed for a display-only X3 and are
    // deliberately stubbed out here. TODO(M3): wire device_info_request reply
    // if the daemon starts gating on it.
  }
}

namespace {

// Land the feed's glance block (weather / usage / wrap-up) in g_state.glance.
// Caller holds the state lock. Absent fields keep their clear() sentinels.
void parseGlanceLocked(JsonObject glance) {
  GlanceInfo& g = g_state.glance;
  g.clear();
  if (glance.isNull()) return;
  g.valid = true;

  auto cp = [](char* d, size_t n, const char* s) {
    strncpy(d, s ? s : "", n - 1);
    d[n - 1] = '\0';
  };
  auto temp = [](JsonVariant v) -> int8_t {
    if (v.isNull()) return GLANCE_TEMP_NONE;
    return (int8_t)(int)v.as<int>();
  };

  JsonObject w = glance["weather"].as<JsonObject>();
  if (!w.isNull()) {
    g.weather.valid = true;
    cp(g.weather.place, sizeof(g.weather.place), w["place"] | "");
    g.weather.tempC = temp(w["tempC"]);
    cp(g.weather.summary, sizeof(g.weather.summary), w["summary"] | "");
    g.weather.todayMinC = temp(w["todayMinC"]);
    g.weather.todayMaxC = temp(w["todayMaxC"]);
    JsonObject rain = w["rain"].as<JsonObject>();
    if (!rain.isNull()) {
      cp(g.weather.rainStartHm, sizeof(g.weather.rainStartHm), rain["startHm"] | "");
      cp(g.weather.rainEndHm, sizeof(g.weather.rainEndHm), rain["endHm"] | "");
      g.weather.rainProbability = (int8_t)(rain["probability"] | -1);
    }
    JsonObject tm = w["tomorrow"].as<JsonObject>();
    if (!tm.isNull()) {
      cp(g.weather.tomorrow.summary, sizeof(g.weather.tomorrow.summary), tm["summary"] | "");
      g.weather.tomorrow.minC = temp(tm["minC"]);
      g.weather.tomorrow.maxC = temp(tm["maxC"]);
      g.weather.tomorrow.rainProbability = (int8_t)(tm["rainProbability"] | -1);
    }
  }

  for (JsonObject u : glance["usage"].as<JsonArray>()) {
    if (g.usageCount >= GlanceInfo::USAGE_CAP) break;
    GlanceUsageRow& row = g.usage[g.usageCount++];
    cp(row.provider, sizeof(row.provider), u["provider"] | "");
    cp(row.label, sizeof(row.label), u["label"] | "");
    row.primaryPercent = (int8_t)(u["primaryPercent"] | -1);
    row.secondaryPercent = (int8_t)(u["secondaryPercent"] | -1);
    cp(row.primaryResetHm, sizeof(row.primaryResetHm), u["primaryResetHm"] | "");
    row.stale = u["stale"] | false;
  }

  for (JsonVariant line : glance["wrapup"].as<JsonArray>()) {
    if (g.wrapupCount >= GlanceInfo::WRAPUP_CAP) break;
    cp(g.wrapup[g.wrapupCount++], GlanceInfo::WRAPUP_BYTES, line | "");
  }

  // Today's schedule (contract § Glance events): pre-trimmed titles, absolute
  // "HH:MM" only. Absent on older daemons — the section simply doesn't render.
  for (JsonObject e : glance["events"].as<JsonArray>()) {
    if (g.eventCount >= GlanceInfo::EVENT_CAP) break;
    GlanceEvent& ev = g.events[g.eventCount];
    cp(ev.startHm, sizeof(ev.startHm), e["startHm"] | "");
    cp(ev.endHm, sizeof(ev.endHm), e["endHm"] | "");
    cp(ev.title, sizeof(ev.title), e["title"] | "");
    if (ev.title[0] == '\0') {
      ev.clear();
      continue;  // a titleless event renders as noise — drop it
    }
    g.eventCount++;
  }
}

// Parse one daemon-authored module card into the fixed Pocket pool.
// Caller holds g_stateMutex. No heap strings survive this function.
void parsePocketLocked(JsonObject card, PocketCard& out) {
  memset(&out, 0, sizeof(out));
  JsonObject module = card["module"].as<JsonObject>();
  copyStr(out.cardId, sizeof(out.cardId), card["cardId"] | "");
  copyStr(out.actionClass, sizeof(out.actionClass), card["actionClass"] | "info");
  copyStr(out.module, sizeof(out.module), module["module"] | "");
  copyStr(out.title, sizeof(out.title), module["title"] | "POCKET");
  copyStr(out.question, sizeof(out.question), module["question"] | "");
  out.context[0] = '\0';
  for (JsonVariant line : module["context"].as<JsonArray>()) {
    const char* text = line | "";
    const size_t used = strlen(out.context);
    const size_t extra = (used ? 3 : 0) + strlen(text);
    // Context lines are already byte-capped by the daemon. Append only whole
    // lines so a full fixed buffer never slices a multibyte UTF-8 sequence.
    if (used + extra >= sizeof(out.context)) break;
    appendSummary(out.context, sizeof(out.context), text);
  }
  for (JsonObject choice : module["choices"].as<JsonArray>()) {
    if (out.choiceCount >= 3) break;
    PocketChoice& dst = out.choices[out.choiceCount];
    copyStr(dst.id, sizeof(dst.id), choice["id"] | "");
    copyStr(dst.label, sizeof(dst.label), choice["label"] | "");
    if (dst.id[0] && dst.label[0]) out.choiceCount++;
  }
}

}  // namespace

FeedApply applyCardFeed(const char* json, size_t length) {
  FeedApply out;
  if (length > AgentDeckCfg::PROTOCOL_MAX_MSG_BYTES) {
    AgentLog::line("PROTO", "card_feed too large: %u bytes — dropped", (unsigned)length);
    return out;
  }

  // The feed nests each session under cards[i].session; reuse the same field
  // subset the sessions_list filter retains so both transports stay in parity.
  filter.clear();
  filter["type"] = true;
  filter["serverTime"] = true;
  filter["serverHm"] = true;
  filter["nextPullSec"] = true;
  filter["deckSig"] = true;
  filter["unchanged"] = true;
  filter["cards"][0]["cardId"] = true;
  filter["cards"][0]["actionClass"] = true;
  static constexpr const char* kSessionFields[] = {
      "id",        "projectName", "modelName",   "agentType",  "controlMode",   "state",
      "port",      "alive",       "currentTool", "elapsedSec", "question",      "promptType",
      "requestId", "activity",    "currentTask", "goal",       "lastEventText",
  };
  for (const char* f : kSessionFields) filter["cards"][0]["session"][f] = true;
  {
    JsonObject module = filter["cards"][0]["module"].to<JsonObject>();
    for (const char* f : {"module", "title", "question"}) module[f] = true;
    module["context"][0] = true;
    module["choices"][0]["id"] = true;
    module["choices"][0]["label"] = true;
  }
  // Glance (sleep dashboard) block — daemon-rendered, byte-trimmed strings.
  {
    JsonObject gw = filter["glance"]["weather"].to<JsonObject>();
    for (const char* f : {"place", "tempC", "summary", "todayMinC", "todayMaxC"}) gw[f] = true;
    for (const char* f : {"startHm", "endHm", "probability"}) gw["rain"][f] = true;
    for (const char* f : {"summary", "minC", "maxC", "rainProbability"}) gw["tomorrow"][f] = true;
    JsonObject gu = filter["glance"]["usage"][0].to<JsonObject>();
    for (const char* f : {"provider", "label", "primaryPercent", "secondaryPercent", "primaryResetHm", "stale"})
      gu[f] = true;
    filter["glance"]["wrapup"][0] = true;
    JsonObject ge = filter["glance"]["events"][0].to<JsonObject>();
    for (const char* f : {"startHm", "endHm", "title"}) ge[f] = true;
  }
  // Staged firmware advert (contract § Pull OTA) — rides full AND unchanged
  // responses, so a sleeping device learns about an update on any pull.
  filter["fw"]["size"] = true;
  filter["fw"]["md5"] = true;

  doc.clear();
  DeserializationError err = deserializeJson(doc, json, length, DeserializationOption::Filter(filter));
  if (err) {
    AgentLog::line("PROTO", "card_feed JSON error: %s", err.c_str());
    return out;
  }
  JsonObject obj = doc.as<JsonObject>();
  if (strcmp(obj["type"] | "", "card_feed") != 0) {
    AgentLog::line("PROTO", "card_feed rejected: wrong type '%s'", obj["type"] | "");
    return out;
  }

  out.unchanged = obj["unchanged"] | false;
  strncpy(out.deckSig, obj["deckSig"] | "", sizeof(out.deckSig) - 1);
  {
    JsonObject fw = obj["fw"].as<JsonObject>();
    if (!fw.isNull()) {
      out.fwSize = fw["size"] | 0U;
      strncpy(out.fwMd5, fw["md5"] | "", sizeof(out.fwMd5) - 1);
    }
  }

  lockState();
  if (!out.unchanged) {
    // Full feed: replace the deck + glance. The unchanged short-circuit must
    // NOT set dataReceived — its empty cards[] means "keep your cache", and
    // dataReceived=true would let that emptiness mask the persisted deck.
    g_state.dataReceived = true;
    g_state.sessionCount = 0;
    g_state.pocketCount = 0;
    for (JsonObject card : obj["cards"].as<JsonArray>()) {
      JsonObject s = card["session"].as<JsonObject>();
      if (!s.isNull()) {
        if (g_state.sessionCount < AgentDeckCfg::SESSIONS_CAP)
          parseSessionLocked(s, g_state.sessions[g_state.sessionCount++]);
        continue;
      }
      JsonObject module = card["module"].as<JsonObject>();
      if (!module.isNull() && g_state.pocketCount < POCKET_CARD_CAP)
        parsePocketLocked(card, g_state.pocketCards[g_state.pocketCount++]);
    }
    parseGlanceLocked(obj["glance"].as<JsonObject>());
    if (g_state.glance.valid) g_state.glanceAtMs = millis();
  }
  // serverTime re-anchors the daemon-clock estimate exactly like a timeline
  // event's ts would — the drifty-RTC "as of" age heals on every pull, even an
  // unchanged one (clock and cadence are per-pull, not per-content).
  const uint64_t serverTime = obj["serverTime"] | (uint64_t)0;
  if (serverTime > 1700000000000ULL) {
    g_state.daemonEpochSec = (uint32_t)(serverTime / 1000ULL);
    g_state.daemonEpochAtMs = millis();
  }
  const char* serverHm = obj["serverHm"] | "";
  if (strlen(serverHm) == 5) {
    strncpy(g_state.serverHm, serverHm, sizeof(g_state.serverHm) - 1);
    g_state.serverHm[sizeof(g_state.serverHm) - 1] = '\0';
    g_state.serverHmAtMs = millis();
  }
  const uint8_t count = g_state.sessionCount + g_state.pocketCount;
  unlockState();

  out.ok = true;
  out.nextPullSec = obj["nextPullSec"] | 0;
  AgentLog::line("PROTO", "card_feed %s: %u cards nextPull=%us sig=%s", out.unchanged ? "unchanged" : "applied",
                 (unsigned)count, (unsigned)out.nextPullSec, out.deckSig);
  return out;
}

}  // namespace Protocol
}  // namespace AgentDeck
