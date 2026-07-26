#include "AgentDashboardActivity.h"

#include <ESPmDNS.h>
#include <EpdFontFamily.h>
#include <SdCardFont.h>
#include <WiFi.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <new>
#include <string>
#include <vector>

#include <esp_ota_ops.h>

#include "CrossPointSettings.h"
#include "HalGPIO.h"
#include "PowerCycle.h"
#include "SilentRestart.h"
#include "agentdeck/card_class.h"
#include "agentdeck/feed_client.h"
#include "WifiCredentialStore.h"
#include "agentdeck/ota_ws_receiver.h"
#include "activities/network/WifiSelectionActivity.h"
#include "agent/AgentLog.h"
#include "agentdeck/agent_commands.h"
#include "agentdeck/agent_state.h"
#include "agentdeck/eink_dashboard_layout.h"
#include "agentdeck/mdns_discovery.h"
#include "agentdeck/udp_discovery.h"
#include "agentdeck/ws_client.h"
#include "components/UITheme.h"  // GUI (theme) + ThemeMetrics + Rect
#include "components/icons/agentdeck_mark.h"
#include "components/icons/glyph_antigravity.h"
#include "components/icons/glyph_antigravity_16.h"
#include "components/icons/glyph_claude.h"
#include "components/icons/glyph_claude_16.h"
#include "components/icons/glyph_codex.h"
#include "components/icons/glyph_codex_16.h"
#include "components/icons/glyph_openclaw.h"
#include "components/icons/glyph_opencode.h"
#include "fontIds.h"

namespace {
using AgentDeck::AgentState;

// Per-agent creature glyph (src/components/icons/glyph_*.h). MUST be a multiple of
// 8: EInkDisplay::drawImageTransparent reads `width/8` bytes per row, so a width
// like 28 (→ 3 bytes, but convert_icon.py packs 4) desyncs every row into noise.
constexpr int kGlyphPx = 32;

// Canonical 1-bit creature glyph for an agent type → nullptr falls back to a dot.
// Mirrors the AGENT_MONO_GLYPH map (shared/src/svg-renderers/agent-logos.ts). Keep
// every wire agentType covered here — a missing branch silently degrades to a dot.
const uint8_t* glyphForAgent(const char* a) {
  if (!a || !a[0]) return nullptr;
  if (strcmp(a, "claude-code") == 0) return GlyphClaude;
  if (strncmp(a, "codex", 5) == 0) return GlyphCodex;  // codex-cli / codex-app / codex
  if (strcmp(a, "opencode") == 0) return GlyphOpenCode;
  if (strcmp(a, "openclaw") == 0) return GlyphOpenClaw;
  if (strncmp(a, "antigravity", 11) == 0 || strcmp(a, "agy") == 0) return GlyphAntigravity;
  return nullptr;
}

// Format an ISO-8601 resetsAt as a compact "Xd Yh" / "Xh Ym" / "Xm" remaining,
// or "" when the system clock isn't NTP-synced yet or the string won't parse.
// configTime(0,0,…) keeps the zone at UTC so mktime() reads the (UTC "…Z") ISO
// correctly. Best-effort: a missing/unsynced clock just drops the countdown.
bool formatResetRemaining(const char* iso, char* out, size_t cap) {
  if (!out || cap == 0) return false;
  out[0] = '\0';
  if (!iso || !iso[0]) return false;
  const time_t now = time(nullptr);
  if (now < 1700000000) return false;  // ~2023-11 — clock not yet synced
  int Y, Mo, D, H, Mi, S;
  if (sscanf(iso, "%d-%d-%dT%d:%d:%d", &Y, &Mo, &D, &H, &Mi, &S) != 6) return false;
  struct tm tmv = {};
  tmv.tm_year = Y - 1900;
  tmv.tm_mon = Mo - 1;
  tmv.tm_mday = D;
  tmv.tm_hour = H;
  tmv.tm_min = Mi;
  tmv.tm_sec = S;
  const time_t reset = mktime(&tmv);
  if (reset <= now) {
    snprintf(out, cap, "now");
    return true;
  }
  long secs = (long)(reset - now);
  if (secs >= 86400)
    snprintf(out, cap, "%ldd %ldh", secs / 86400, (secs % 86400) / 3600);
  else if (secs >= 3600)
    snprintf(out, cap, "%ldh %ldm", secs / 3600, (secs % 3600) / 60);
  else
    snprintf(out, cap, "%ldm", secs / 60);
  return true;
}

// True when the text contains Hangul / CJK / Kana codepoints — i.e. glyphs the
// Latin-only built-in UI fonts can't render (they'd show □). Used to swap to an
// installed SD CJK font for that line.
bool hasCJK(const char* s) {
  if (!s) return false;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
  while (*p) {
    const unsigned char c = *p;
    if (c < 0x80) {
      p++;
      continue;
    }
    uint32_t cp = 0;
    int n = 0;
    if ((c & 0xE0) == 0xC0) {
      cp = c & 0x1F;
      n = 1;
    } else if ((c & 0xF0) == 0xE0) {
      cp = c & 0x0F;
      n = 2;
    } else if ((c & 0xF8) == 0xF0) {
      cp = c & 0x07;
      n = 3;
    } else {
      p++;
      continue;
    }
    p++;
    for (int i = 0; i < n && *p; i++, p++) cp = (cp << 6) | (*p & 0x3F);
    if ((cp >= 0xAC00 && cp <= 0xD7A3) ||  // Hangul syllables
        (cp >= 0x1100 && cp <= 0x11FF) ||  // Hangul Jamo
        (cp >= 0x3040 && cp <= 0x30FF) ||  // Hiragana + Katakana
        (cp >= 0x4E00 && cp <= 0x9FFF))    // CJK unified ideographs
      return true;
  }
  return false;
}

size_t utf8Span(const char* p) {
  if (!p || !p[0]) return 0;
  const unsigned char c = static_cast<unsigned char>(p[0]);
  size_t n = c < 0x80 ? 1 : ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : 4));
  for (size_t i = 1; i < n; i++)
    if (!p[i] || (static_cast<unsigned char>(p[i]) & 0xC0) != 0x80) return 1;
  return n;
}

void removeLastUtf8(char* text) {
  size_t n = strlen(text);
  if (n == 0) return;
  n--;
  while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80) n--;
  text[n] = '\0';
}

bool hasVisibleText(const char* p) {
  while (p && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
  return p && *p;
}

// Draw bounded UTF-8 text without std::string/vector allocations. This is used
// by every overview card during a render pass, so it must not fragment the C3's
// no-PSRAM heap. Returns the number of lines drawn.
int drawWrappedFixed(const GfxRenderer& renderer, int fontId, int x, int y, const char* text, int maxWidth,
                     int maxLines, int lineAdvance, EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  if (!text || !text[0] || maxWidth <= 0 || maxLines <= 0) return 0;
  const char* p = text;
  int drawn = 0;
  while (hasVisibleText(p) && drawn < maxLines) {
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    char line[AgentDeck::SESSION_ACTIVITY_CAP];
    line[0] = '\0';
    size_t scan = 0, lastFit = 0, lastSpace = 0;
    bool explicitBreak = false;
    while (p[scan]) {
      if (p[scan] == '\n' || p[scan] == '\r') {
        explicitBreak = true;
        break;
      }
      const size_t cp = utf8Span(p + scan);
      if (cp == 0 || scan + cp >= sizeof(line)) break;
      memcpy(line + scan, p + scan, cp);
      line[scan + cp] = '\0';
      if (renderer.getTextWidth(fontId, line, style) > maxWidth) break;
      if (p[scan] == ' ') lastSpace = scan;
      scan += cp;
      lastFit = scan;
    }

    size_t chosen = lastFit;
    size_t consumed = scan;
    if (p[scan] && !explicitBreak) {
      if (lastSpace > 0) {
        chosen = lastSpace;
        consumed = lastSpace + 1;
      } else {
        consumed = lastFit;
      }
    } else if (explicitBreak) {
      consumed = scan + 1;
    }
    if (chosen == 0 && p[0]) {
      chosen = utf8Span(p);
      if (chosen >= sizeof(line)) chosen = sizeof(line) - 1;
      memcpy(line, p, chosen);
      consumed = chosen;
    }
    line[chosen] = '\0';
    while (chosen > 0 && line[chosen - 1] == ' ') line[--chosen] = '\0';

    const bool more = hasVisibleText(p + consumed);
    if (drawn == maxLines - 1 && more) {
      constexpr const char* dots = "...";
      while (line[0]) {
        char candidate[AgentDeck::SESSION_ACTIVITY_CAP];
        snprintf(candidate, sizeof(candidate), "%s%s", line, dots);
        if (renderer.getTextWidth(fontId, candidate, style) <= maxWidth) break;
        removeLastUtf8(line);
      }
      strncat(line, dots, sizeof(line) - strlen(line) - 1);
    }
    renderer.drawText(fontId, x, y + drawn * lineAdvance, line, true, style);
    drawn++;
    if (consumed == 0) break;
    p += consumed;
  }
  return drawn;
}

// Strip an "observed:<agent>:" prefix → the raw session UUID. The sessions_list
// id for passively-observed sessions is prefixed ("observed:claude:<uuid>") while
// timeline entries are keyed by the raw UUID, so Detail must compare the raw form.
const char* rawSid(const char* sid) {
  if (sid && strncmp(sid, "observed:", 9) == 0) {
    const char* p = strchr(sid + 9, ':');
    if (p) return p + 1;
  }
  return sid ? sid : "";
}

// Compact, uppercase status badge for the overview rows.
const char* stateBadge(const char* s) {
  if (!s || !s[0]) return "—";
  if (strncmp(s, "awaiting", 8) == 0) return "AWAITING";
  if (strcmp(s, "processing") == 0) return "WORKING";
  if (strcmp(s, "idle") == 0) return "IDLE";
  if (strcmp(s, "disconnected") == 0) return "OFFLINE";
  return s;
}

// FNV-1a over a byte range — cheap change-detection signature.
inline uint32_t fnvUpdate(uint32_t h, const void* data, size_t len) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < len; i++) {
    h ^= p[i];
    h *= 16777619u;
  }
  return h;
}

const char* agentStateLabel(AgentState s) {
  switch (s) {
    case AgentState::IDLE:
      return "Idle";
    case AgentState::PROCESSING:
      return "Working";
    case AgentState::AWAITING_PERMISSION:
      return "Awaiting permission";
    case AgentState::AWAITING_OPTION:
      return "Choosing option";
    case AgentState::AWAITING_DIFF:
      return "Reviewing diff";
    case AgentState::DISCONNECTED:
    default:
      // Same vocabulary as stateBadge()'s "OFFLINE" — the Overview badge and
      // the Detail meta line describe the same wire state with one word.
      return "Offline";
  }
}

// Prose form of a raw wire state for the Detail meta line — same vocabulary as
// stateBadge() so Overview ("WORKING") and Detail ("Working") never diverge.
// Unknown strings pass through unchanged (already-pretty fallback labels).
const char* wireStateLabel(const char* s) {
  if (!s || !s[0]) return "";
  if (strcmp(s, "processing") == 0) return "Working";
  if (strcmp(s, "idle") == 0) return "Idle";
  if (strcmp(s, "awaiting_permission") == 0) return "Awaiting permission";
  if (strcmp(s, "awaiting_option") == 0) return "Choosing option";
  if (strcmp(s, "awaiting_diff") == 0) return "Reviewing diff";
  if (strcmp(s, "disconnected") == 0) return "Offline";
  return s;
}

// E-ink timeline marker per entry type. Vocabulary is the shared SSOT
// EINK_ICON_GLYPHS in AgentDeck shared/src/timeline-icons.ts (mirrored on the
// Android e-ink surface) — keep in lockstep when the icon-key mapping changes.
const char* timelineGlyph(const char* type) {
  if (!type || !type[0]) return "[..]";
  if (strcmp(type, "chat_start") == 0) return "[..]";
  if (strcmp(type, "chat_end") == 0 || strcmp(type, "chat_response") == 0 || strcmp(type, "model_response") == 0 ||
      strcmp(type, "tool_resolved") == 0 || strcmp(type, "task_milestone") == 0 || strcmp(type, "eval_result") == 0)
    return "[OK]";
  if (strcmp(type, "task_start") == 0 || strcmp(type, "task_end") == 0) return "[==]";
  if (strcmp(type, "error") == 0) return "[!!]";
  if (strcmp(type, "tool_request") == 0) return "[??]";
  if (strcmp(type, "tool_exec") == 0) return "[T ]";
  if (strcmp(type, "model_call") == 0) return "[M ]";
  if (strcmp(type, "user_action") == 0) return "[U ]";
  if (strcmp(type, "scheduled") == 0) return "[S ]";
  if (strcmp(type, "memory_recall") == 0) return "[~ ]";
  return "[..]";
}

// Compact age ("now" / "5m" / "2h" / "3d") from seconds. Buffer >= 6 bytes.
void formatAge(uint32_t ageSec, char* out, size_t n) {
  if (ageSec < 60)
    snprintf(out, n, "now");
  else if (ageSec < 3600)
    snprintf(out, n, "%um", (unsigned)(ageSec / 60));
  else if (ageSec < 86400)
    snprintf(out, n, "%uh", (unsigned)(ageSec / 3600));
  else
    snprintf(out, n, "%ud", (unsigned)(ageSec / 86400));
}

const char* deviceModelName() { return gpio.deviceIsX3() ? "XTeink X3" : "XTeink X4"; }

const char* deviceModelSlug() { return gpio.deviceIsX3() ? "xteink-x3" : "xteink-x4"; }

const char* mdnsHostName() { return gpio.deviceIsX3() ? "agentdeck-x3" : "agentdeck-x4"; }
}  // namespace

void AgentDashboardActivity::onEnter() {
  Activity::onEnter();

  dashState = DashState::WifiSelection;
  localIp.clear();
  exitRequested = false;
  registered = false;
  lastSignature = 0;

  // Bring the networking module up from a clean slate.
  AgentDeck::ensureStateMutex();
  AgentDeck::lockState();
  AgentDeck::g_state.reset();
  AgentDeck::unlockState();
  AgentDeck::Net::wsInit();

  // CJK font so Korean renders instead of □. Prefer the bundled Noto Sans KR
  // shipped on the SD (works with zero user setup); else fall back to the reader's
  // font when the user happens to have a CJK family selected.
  cjkFontId = loadKoreanFont();
  if (cjkFontId == 0 && SETTINGS.sdFontFamilyName[0] != '\0') cjkFontId = SETTINGS.getReaderFontId();

  // M5.5: load the persisted deck BEFORE the first paint so boot lands on the
  // last-synced cards (with an "as of" age), not an empty screen. Display-only —
  // cached rows never gain buttons; decisions require the live daemon.
  cachedDeck.reset(new (std::nothrow) AgentDeck::DeckStore::Snapshot());
  if (cachedDeck && !AgentDeck::DeckStore::load(*cachedDeck)) cachedDeck->count = 0;
  lastDeckSig = 0;
  lastDeckSaveMs = 0;
  clockSynced = time(nullptr) >= 1700000000;

  // M6 battery cadence: a timer wake with the setting enabled syncs once over
  // HTTP and deep-sleeps again; any button press cancels into interactive mode.
  // USB power means docked — stay in the live WS mode regardless of the timer.
  enterMs = millis();
  pullMode = SETTINGS.agentPullSyncEnabled != 0 &&
             SETTINGS.startupApp == CrossPointSettings::STARTUP_AGENTDECK &&
             gpio.getWakeupReason() == HalGPIO::WakeupReason::Timer && !gpio.isUsbConnected();
  pullSynced = false;
  pullEndpointTried = false;
  timedSleepImminent = false;
  pullSyncedAtMs = 0;
  pullNextSec = 0;
  if (pullMode) AgentLog::line("AGENT", "pull-sync wake (battery cadence)");

  AgentLog::line("AGENT", "AgentDashboardActivity onEnter (Face shell)");
  // Paint the Face immediately — content first, connection is a status line.
  requestUpdate();

  if (WiFi.status() == WL_CONNECTED) {
    localIp = WiFi.localIP().toString().c_str();
    onWifiSelectionComplete(true);
    return;
  }

  // Saved credentials → background STA join, no blocking picker. The Face is
  // already on screen; loop() promotes the state when the join lands.
  const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
  const WifiCredential* cred = lastSsid.empty() ? nullptr : WIFI_STORE.findCredential(lastSsid);
  if (cred) {
    strncpy(joiningSsid, cred->ssid.c_str(), sizeof(joiningSsid) - 1);
    joiningSsid[sizeof(joiningSsid) - 1] = '\0';
    AgentLog::line("AGENT", "background wifi join: %s", joiningSsid);
    WiFi.mode(WIFI_STA);
    if (cred->password.empty())
      WiFi.begin(cred->ssid.c_str());
    else
      WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
    dashState = DashState::WifiJoining;
    wifiJoinStartMs = millis();
    return;
  }

  // First run (no saved network): the picker genuinely needs the screen.
  launchWifiPicker();
}

void AgentDashboardActivity::launchWifiPicker() {
  dashState = DashState::WifiSelection;
  requestUpdate();
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (!result.isCancelled) {
                             const auto& wifi = std::get<WifiResult>(result.data);
                             localIp = wifi.ip;
                           }
                           onWifiSelectionComplete(!result.isCancelled);
                         });
}

void AgentDashboardActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    AgentLog::line("AGENT", "wifi selection cancelled — exiting");
    finish();
    return;
  }
  if (localIp.empty()) localIp = WiFi.localIP().toString().c_str();
  AgentLog::line("AGENT", "wifi up: %s", localIp.c_str());
  startNetworking();
}

void AgentDashboardActivity::startNetworking() {
  // Best-effort clock for the LIMITS reset countdowns. Non-blocking — SNTP updates
  // the system time in the background; until it lands, formatResetRemaining() just
  // omits the countdown. UTC offset 0 so mktime() reads the ISO "…Z" resetsAt right.
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  AgentDeck::Net::mdnsInit(mdnsHostName());
  // Bind the UDP discovery socket. WiFi may not be up yet on the first call —
  // udpInit() is idempotent, so the loop() also retries each tick while we
  // remain in Discovering. This handles the race where mdnsInit() runs before
  // WiFi STA completes its handshake.
  AgentDeck::Net::udpInit();
  dashState = DashState::Discovering;
  discoveryStartMs = millis();
  discoveryNoticeShown = false;
  requestUpdate();
}

void AgentDashboardActivity::sendClientRegister() {
  // {"type":"client_register","clientType":"eink-device","clientLabel":"XTeink X4",
  //  "devices":[{"id":"<mac>","name":"XTeink X4","family":"xteink-x4","columns":W,"rows":H}]}
  String mac = WiFi.macAddress();
  const char* modelName = deviceModelName();
  const char* modelSlug = deviceModelSlug();
  char buf[256];
  int n = snprintf(buf, sizeof(buf),
                   "{\"type\":\"client_register\",\"clientType\":\"eink-device\","
                   "\"clientLabel\":\"%s\",\"devices\":[{\"id\":\"%s\","
                   "\"name\":\"%s\",\"family\":\"%s\",\"columns\":%d,\"rows\":%d}]}",
                   modelName, mac.c_str(), modelName, modelSlug, renderer.getScreenWidth(), renderer.getScreenHeight());
  if (n > 0 && (size_t)n < sizeof(buf)) {
    AgentDeck::Net::wsSend(buf);
    AgentLog::line("AGENT", "client_register sent model=%s family=%s mac=%s", modelName, modelSlug, mac.c_str());
  }
  // Ask for initial usage; the daemon pushes state_update/sessions_list on connect.
  AgentDeck::Net::wsSend("{\"type\":\"query_usage\"}");
}

void AgentDashboardActivity::sendDeviceInfo() {
  // Announce as a first-class AgentDeck ESP32 device so the daemon registers it
  // in its device registry (Node daemon: type "esp32-wifi"; see AgentDeck
  // docs/esp32-client-contract.md). This is complementary to sendClientRegister():
  // the eink-device roster drives the macOS Dashboard E-ink rail, while device_info
  // drives the ESP32 device registry + OTA identity across all daemons. The board
  // wire string uses the underscore convention (ips_10, ulanzi_tc001, …), distinct
  // from the hyphen family slug in client_register.
  //
  // AgentDeck WiFi OTA v1 supported (src/agentdeck/ota_ws_receiver.*): chunks
  // stream to an SD cache, then flash via the raw-partition path (NOT the
  // Arduino Update class — X4 silicon rejects the patched image through
  // esp_image_verify). Slot capability comes from the live partition table.
  const char* board = gpio.deviceIsX3() ? "xteink_x3" : "xteink_x4";
  String ip = WiFi.localIP().toString();
  uint8_t timelineCount = 0, sessionCount = 0;
  AgentDeck::lockState();
  timelineCount = AgentDeck::g_state.timelineCount;
  sessionCount = AgentDeck::g_state.sessionCount;
  AgentDeck::unlockState();
  const esp_partition_t* otaDest = esp_ota_get_next_update_partition(nullptr);
  const unsigned otaSlotSize = otaDest ? (unsigned)otaDest->size : 0;
  // buildHash = trailing token of CROSSPOINT_VERSION ("1.4.1-dev-master-<sha>").
  const char* buildHash = strrchr(CROSSPOINT_VERSION, '-');
  buildHash = buildHash ? buildHash + 1 : CROSSPOINT_VERSION;
  char buf[512];
  int n = snprintf(buf, sizeof(buf),
                   "{\"type\":\"device_info\",\"board\":\"%s\",\"version\":\"%s\",\"buildHash\":\"%s\","
                   "\"protocolRevision\":%u,\"wifiConfigured\":true,\"wifiConnected\":true,"
                   "\"ip\":\"%s\",\"otaSupported\":%s,\"otaSlotCount\":2,\"otaSlotSize\":%u,"
                   "\"otaFreeSketchSpace\":%u,"
                   "\"timelineCount\":%u,\"sessionCount\":%u}",
                   board, CROSSPOINT_VERSION, buildHash, (unsigned)AgentDeckCfg::PROTOCOL_REVISION, ip.c_str(),
                   otaDest ? "true" : "false", otaSlotSize, otaSlotSize, (unsigned)timelineCount,
                   (unsigned)sessionCount);
  if (n > 0 && (size_t)n < sizeof(buf)) {
    AgentDeck::Net::wsSend(buf);
    AgentLog::line("AGENT", "device_info sent board=%s ver=%s ip=%s", board, CROSSPOINT_VERSION, ip.c_str());
  }
}

uint32_t AgentDashboardActivity::computeStateSignature() const {
  uint32_t h = 2166136261u;
  AgentDeck::lockState();
  const auto& s = AgentDeck::g_state;
  uint8_t st = static_cast<uint8_t>(s.state);
  h = fnvUpdate(h, &st, sizeof(st));
  h = fnvUpdate(h, &s.wsConnected, sizeof(s.wsConnected));
  h = fnvUpdate(h, &s.dataReceived, sizeof(s.dataReceived));
  h = fnvUpdate(h, &s.sessionCount, sizeof(s.sessionCount));
  h = fnvUpdate(h, s.projectName, strlen(s.projectName));
  h = fnvUpdate(h, s.question, strlen(s.question));
  h = fnvUpdate(h, s.currentTool, strlen(s.currentTool));
  h = fnvUpdate(h, &s.optionCount, sizeof(s.optionCount));
  h = fnvUpdate(h, s.optionSessionId, strlen(s.optionSessionId));
  h = fnvUpdate(h, &s.navigable, sizeof(s.navigable));
  for (uint8_t i = 0; i < s.optionCount && i < 8; i++) {
    h = fnvUpdate(h, s.options[i].label, strlen(s.options[i].label));
    h = fnvUpdate(h, s.options[i].action, strlen(s.options[i].action));
    h = fnvUpdate(h, &s.options[i].index, sizeof(s.options[i].index));
  }
  h = fnvUpdate(h, s.requestId, strlen(s.requestId));
  // Per-session state so the triage list repaints when any session's awaiting
  // status (or its prompt) changes, even if the focused state_update didn't.
  for (uint8_t i = 0; i < s.sessionCount && i < AgentDeckCfg::SESSIONS_CAP; i++) {
    h = fnvUpdate(h, s.sessions[i].id, strlen(s.sessions[i].id));
    h = fnvUpdate(h, s.sessions[i].controlMode, strlen(s.sessions[i].controlMode));
    h = fnvUpdate(h, s.sessions[i].state, strlen(s.sessions[i].state));
    h = fnvUpdate(h, s.sessions[i].requestId, strlen(s.sessions[i].requestId));
    h = fnvUpdate(h, s.sessions[i].activity, strlen(s.sessions[i].activity));
  }
  // Codex limits + timeline revision so the footer / Detail repaint even when
  // queried history replaces a same-sized ring (head/count alone can match).
  h = fnvUpdate(h, &s.codexFivePercent, sizeof(s.codexFivePercent));
  h = fnvUpdate(h, &s.codexSevenPercent, sizeof(s.codexSevenPercent));
  h = fnvUpdate(h, &s.timelineRevision, sizeof(s.timelineRevision));
  // Usage so the LIMITS footer repaints when a quota gauge / reset / subscription moves.
  h = fnvUpdate(h, &s.fiveHourPercent, sizeof(s.fiveHourPercent));
  h = fnvUpdate(h, &s.sevenDayPercent, sizeof(s.sevenDayPercent));
  h = fnvUpdate(h, &s.usageStale, sizeof(s.usageStale));
  h = fnvUpdate(h, s.fiveHourReset, strlen(s.fiveHourReset));
  h = fnvUpdate(h, s.sevenDayReset, strlen(s.sevenDayReset));
  h = fnvUpdate(h, s.codexPlan, strlen(s.codexPlan));
  h = fnvUpdate(h, s.codexActiveUntil, strlen(s.codexActiveUntil));
  h = fnvUpdate(h, s.antigravityPlan, strlen(s.antigravityPlan));
  h = fnvUpdate(h, &s.antigravityCredits, sizeof(s.antigravityCredits));
  AgentDeck::unlockState();
  // Local view/cursor state so navigation repaints.
  uint8_t vm = static_cast<uint8_t>(viewMode);
  h = fnvUpdate(h, &vm, sizeof(vm));
  h = fnvUpdate(h, &overviewCursor, sizeof(overviewCursor));
  h = fnvUpdate(h, &triageIndex, sizeof(triageIndex));
  h = fnvUpdate(h, &optionCursor, sizeof(optionCursor));
  return h;
}

void AgentDashboardActivity::loop() {
  // Abort a stalled OTA receive (sender died mid-push) so `receiving()` can't
  // swallow input forever. Runs before the receive/flash checks below.
  AgentDeck::OtaWs::service();

  // OTA: a fully-received, validated image is waiting. Paint the blocking
  // notice, then flash + restart from this (main) task — never from the WS
  // callback. serviceFlash() only returns on failure.
  if (AgentDeck::OtaWs::flashPending()) {
    otaFlashNotice = true;
    requestUpdateAndWait();
    AgentDeck::OtaWs::serviceFlash();
    otaFlashNotice = false;
    requestUpdate();
    return;
  }
  // Receive-phase progress: repaint the Face status line in 5% steps (e-ink).
  if (AgentDeck::OtaWs::receiving()) {
    const uint32_t total = AgentDeck::OtaWs::totalBytes();
    const int bucket = total ? (int)((uint64_t)AgentDeck::OtaWs::receivedBytes() * 20 / total) : 0;
    if (bucket != otaPctBucket) {
      otaPctBucket = bucket;
      requestUpdate();
    }
  } else {
    otaPctBucket = -1;
  }

  // Handle input FIRST so Back stays responsive: the discovery/connect steps
  // below can block (mDNS queryService ~1s, WS connect) and would otherwise
  // starve the button poll, making "go back" feel dead while not yet connected.
  handleButtons();
  if (exitRequested) {
    finish();
    return;
  }
  // Presence cancels the cadence pass: the user pressed something, so this
  // wake continues into the normal interactive WS flow instead of sleeping.
  if (pullMode && lastUserInputMs != 0) {
    AgentLog::line("AGENT", "pull-sync cancelled by input — interactive mode");
    pullMode = false;
    requestUpdate();
  }

  if (dashState == DashState::WifiJoining) {
    // Background STA join in progress. The Face is already on screen; promote
    // to daemon discovery when the join lands, or fall back to the interactive
    // picker after the budget (wrong password, AP gone, …).
    if (WiFi.status() == WL_CONNECTED) {
      localIp = WiFi.localIP().toString().c_str();
      WIFI_STORE.setLastConnectedSsid(joiningSsid);
      AgentLog::line("AGENT", "wifi joined %s (%s)", joiningSsid, localIp.c_str());
      startNetworking();
    } else if (millis() - wifiJoinStartMs > kWifiJoinTimeoutMs) {
      if (pullMode) {
        // Unattended wake must never end on an interactive picker: give up
        // this cycle and retry on the next timer wake.
        AgentLog::line("AGENT", "wifi join timeout in pull mode — sleeping");
        beginTimedSleep(kPullDefaultSec);
        return;
      }
      AgentLog::line("AGENT", "wifi join timeout (%s) — opening picker", joiningSsid);
      WiFi.disconnect(true);
      launchWifiPicker();
    }
    return;
  }

  if (dashState == DashState::Discovering) {
    // Retry UDP socket bind if WiFi came up after startNetworking(). Idempotent.
    AgentDeck::Net::udpInit();

    // M6 pull mode: cached-endpoint fast path — don't spend the battery window
    // on mDNS when the daemon rarely moves. Failure (daemon restarted onto a
    // different port) falls through to normal discovery below.
    if (pullMode && !pullSynced && !pullEndpointTried) {
      pullEndpointTried = true;
      char ip[16] = {0};
      char token[40] = {0};
      uint16_t port = 0;
      if (AgentDeck::Feed::loadEndpoint(ip, sizeof(ip), port, token, sizeof(token))) {
        attemptPullSync(ip, port, token);
      }
    }

    AgentDeck::Net::BridgeInfo bridge;
    // mDNS first (canonical TXT-derived ip, daemon/canonical-port priority),
    // then fall back to the UDP beacon — same BridgeInfo shape, lower trust
    // because anyone on the subnet can broadcast, but the remoteIP/subnet
    // guards in udpPoll() keep it safe.
    bool found = !pullSynced && AgentDeck::Net::mdnsPoll(bridge);
    if (!found && !pullSynced) found = AgentDeck::Net::udpPoll(bridge);
    if (found && bridge.found) {
      if (pullMode) {
        // Pull mode answers discovery with one HTTP sync, not a WS connect.
        attemptPullSync(bridge.ip, bridge.port, bridge.token);
      } else {
        AgentLog::line("AGENT", "daemon @ %s:%u (agent=%s) — connecting", bridge.ip, (unsigned)bridge.port,
                       bridge.agent);
        AgentDeck::Net::wsConnect(bridge.ip, bridge.port, bridge.token);
        dashState = DashState::Connecting;
        connectStartMs = millis();
        discoveryNoticeShown = false;
        requestUpdate();
      }
    } else if (!discoveryNoticeShown && millis() - discoveryStartMs >= kDiscoveryNotFoundMs) {
      discoveryNoticeShown = true;
      requestUpdate();
    }
  } else if (dashState == DashState::Connecting || dashState == DashState::Connected) {
    AgentDeck::Net::wsLoop();
    AgentDeck::Net::pumpOutbound();

    const bool nowConnected = AgentDeck::Net::wsConnected();
    if (nowConnected && dashState == DashState::Connecting) {
      dashState = DashState::Connected;
      lastConnectedMs = millis();
      if (!registered) {
        sendClientRegister();
        sendDeviceInfo();
        registered = true;
      }
      // Cache the live endpoint for the M6 pull cadence: a later timer wake
      // syncs against it over HTTP without re-running discovery.
      AgentDeck::Feed::saveEndpoint(AgentDeck::Net::wsBridgeIp(), AgentDeck::Net::wsBridgePort(),
                                    AgentDeck::Net::wsBridgeToken());
      requestUpdate();
    } else if (!nowConnected && dashState == DashState::Connecting) {
      // The cached ip:port isn't accepting — most likely the daemon moved to a
      // different port (dynamic 9120→fallback). Don't sit on a stale endpoint:
      // after a grace window, re-resolve fresh via mDNS.
      if (millis() - connectStartMs > kConnectTimeoutMs) {
        AgentLog::line("AGENT", "connect timeout — re-resolving via mDNS");
        AgentDeck::Net::wsDisconnect();  // clears saved ip:port, stops stale auto-reconnect
        AgentDeck::Net::mdnsRefresh();   // force an immediate fresh query
        dashState = DashState::Discovering;
        discoveryStartMs = millis();
        discoveryNoticeShown = false;
        registered = false;
        requestUpdate();
      }
    } else if (!nowConnected && dashState == DashState::Connected) {
      const uint32_t uptime = millis() - lastConnectedMs;
      registered = false;
      if (uptime >= kHealthyUptimeMs) {
        // Was a healthy connection that dropped (transient / daemon restart on the
        // same port): retry the SAME endpoint — the ws_client library auto-reconnects
        // to it. Avoids flapping across multiple daemons on the LAN. If it stays
        // unreachable, the connect-timeout above falls back to a fresh mDNS resolve.
        AgentLog::line("AGENT", "ws dropped after %ums — retrying same endpoint", (unsigned)uptime);
        dashState = DashState::Connecting;
        connectStartMs = millis();
      } else {
        // Endpoint accepted then dropped us quickly — a flaky/duplicate daemon.
        // Re-resolve to try a different advertiser instead of hammering this one.
        AgentLog::line("AGENT", "ws dropped after %ums — re-resolving (flaky endpoint)", (unsigned)uptime);
        AgentDeck::Net::wsDisconnect();
        AgentDeck::Net::mdnsRefresh();
        dashState = DashState::Discovering;
        discoveryStartMs = millis();
        discoveryNoticeShown = false;
      }
      requestUpdate();
    }

    // Repaint only when the rendered state actually changed. Throttled: the loop
    // runs with skipLoopDelay(), so an unthrottled check would re-hash all sessions,
    // timeline entries, and usage strings under g_stateMutex thousands of times per
    // second (pure CPU/power waste + mutex contention with the render task). The
    // interval also coalesces rapid state_update bursts into ≤2 repaints/sec, which
    // is all the e-ink panel can usefully show anyway.
    if (dashState == DashState::Connected && millis() - lastSigCheckMs >= kSigCheckIntervalMs) {
      lastSigCheckMs = millis();
      serviceCard();  // auto-surface / auto-resolve the Decision Card first, so
                      // the signature check below repaints the flip in this tick
      serviceDeckPersist();  // M5.5: keep the SD deck cache in sync (throttled)
      uint32_t sig = computeStateSignature();
      if (sig != lastSignature) {
        lastSignature = sig;
        requestUpdate();
      }
    }
  }

  // SNTP landing upgrades the cached Face's "as of" line from bare to an actual
  // age — repaint once on that transition (it happens at most once per boot).
  const bool nowSynced = time(nullptr) >= 1700000000;
  if (nowSynced != clockSynced) {
    clockSynced = nowSynced;
    requestUpdate();
  }

  // ── M6 power ladder: decide whether this wake goes back to sleep ──
  if (pullMode) {
    servicePullSync();
  } else {
    serviceIdleCadence();
  }
}

bool AgentDashboardActivity::attemptPullSync(const char* ip, uint16_t port, const char* token) {
  const char* board = gpio.deviceIsX3() ? "xteink_x3" : "xteink_x4";
  const auto r = AgentDeck::Feed::syncOnce(ip, port, token, board);
  if (!r.ok) return false;
  pullSynced = true;
  pullSyncedAtMs = millis();
  pullNextSec = r.nextPullSec;
  // Persist immediately (unthrottled): the frozen Face and the wake-time cache
  // must agree on what was just pulled.
  lastDeckSaveMs = 0;
  serviceDeckPersist();
  requestUpdate();
  return true;
}

void AgentDashboardActivity::servicePullSync() {
  // Never sleep out from under a firmware transfer.
  if (AgentDeck::OtaWs::receiving() || AgentDeck::OtaWs::flashPending()) return;
  const uint32_t now = millis();
  if (pullSynced) {
    // Linger briefly so a present user can grab the device (any press cancels
    // pull mode above); then freeze the Face and sleep until the next pull.
    if (now - pullSyncedAtMs >= kPullLingerMs) {
      beginTimedSleep(pullNextSec ? pullNextSec : kPullDefaultSec);
    }
    return;
  }
  // Unsynced: Wi-Fi joined but the daemon never answered within the budget —
  // don't burn the battery scanning; retry on the next cadence tick.
  if (now - enterMs >= kPullBudgetMs) {
    AgentLog::line("AGENT", "pull-sync budget exhausted — sleeping unsynced");
    beginTimedSleep(kPullDefaultSec);
  }
}

void AgentDashboardActivity::serviceIdleCadence() {
  if (SETTINGS.agentPullSyncEnabled == 0 || SETTINGS.startupApp != CrossPointSettings::STARTUP_AGENTDECK) return;
  if (gpio.isUsbConnected()) return;              // docked → stay in the live WS mode
  if (dashState != DashState::Connected) return;  // pre-connected states keep their own budgets
  if (viewMode != ViewMode::Overview) return;     // never sleep under a Card/Detail
  if (AgentDeck::OtaWs::receiving() || AgentDeck::OtaWs::flashPending()) return;
  const uint32_t idleAnchor = lastUserInputMs ? lastUserInputMs : enterMs;
  if (millis() - idleAnchor < kIdleToCadenceMs) return;
  // A session that needs the user keeps the Face live — a frozen prompt the
  // buttons can't answer would violate the attention contract's honesty rule.
  AwaitingItem pending;
  if (firstCardCandidate(pending)) return;
  // Mid-turn sessions shorten the cadence (the daemon's nextPullSec hint uses
  // the same rule server-side; mirrored here because WS mode has no hint).
  bool anyActive = false;
  AgentDeck::lockState();
  for (uint8_t i = 0; i < AgentDeck::g_state.sessionCount; i++) {
    const auto& s = AgentDeck::g_state.sessions[i];
    if (s.alive && (strcmp(s.state, "processing") == 0 || strncmp(s.state, "awaiting", 8) == 0)) {
      anyActive = true;
      break;
    }
  }
  AgentDeck::unlockState();
  AgentLog::line("AGENT", "idle on battery — entering pull cadence");
  beginTimedSleep(anyActive ? kPullActiveSec : kPullDefaultSec);
}

void AgentDashboardActivity::beginTimedSleep(uint32_t seconds) {
  if (seconds < kPullMinSec) seconds = kPullMinSec;
  // Final deck persist (unthrottled) so the frozen Face and the cache agree.
  lastDeckSaveMs = 0;
  serviceDeckPersist();
  // Paint the Face one last time with the sleeping status so the retained
  // frame is honest about being a snapshot.
  timedSleepImminent = true;
  requestUpdateAndWait();
  AgentLog::line("AGENT", "timed deep sleep: %us", (unsigned)seconds);
  enterTimedDeepSleep(seconds, bestEpochNow());
}

void AgentDashboardActivity::onExit() {
  Activity::onExit();

  AgentDeck::Net::wsDisconnect();
  AgentDeck::Net::udpStop();
  AgentDeck::Net::mdnsStop();
  MDNS.end();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();  // defrag → Home (mirrors CalibreConnectActivity::onExit)
  }
}

bool AgentDashboardActivity::findAwaiting(const char* selected, AwaitingItem& out) const {
  auto cp = [](char* d, size_t n, const char* s) {
    strncpy(d, s, n - 1);
    d[n - 1] = '\0';
  };
  bool found = false;
  AgentDeck::lockState();
  const auto& s = AgentDeck::g_state;
  for (uint8_t i = 0; i < s.sessionCount; i++) {
    const auto& se = s.sessions[i];
    if (strncmp(se.state, "awaiting", 8) != 0 || !selected || strcmp(se.id, selected) != 0) continue;
    cp(out.sid, sizeof(out.sid), se.id);
    cp(out.requestId, sizeof(out.requestId), se.requestId);
    // Options are actionable only when the daemon explicitly identifies this
    // session as their owner. Merely being focused is not enough: aggregate
    // state_update packets can focus an observed session while carrying options
    // left over from another managed PTY.
    const bool optionsCorrelated = s.optionSessionId[0] != '\0' && strcmp(se.id, s.optionSessionId) == 0;
    cp(out.question, sizeof(out.question), optionsCorrelated && s.question[0] ? s.question : se.question);
    out.optionCount = optionsCorrelated ? s.optionCount : 0;
    out.attentionMode = AgentDeck::classifyAttention(true, AgentDeck::isObservedSession(se.controlMode, se.id),
                                                     se.requestId[0] != '\0', optionsCorrelated, out.optionCount);
    found = true;
    break;
  }
  // Focused-state fallback: sessions_list can lag a just-arrived state_update,
  // so accept it only when its session id is the Detail row being inspected.
  if (!found && (s.state == AgentState::AWAITING_PERMISSION || s.state == AgentState::AWAITING_OPTION ||
                 s.state == AgentState::AWAITING_DIFF)) {
    const char* stateSid = s.sessionId[0] ? s.sessionId : s.focusedSessionId;
    if (!selected || strcmp(stateSid, selected) == 0) {
      cp(out.sid, sizeof(out.sid), stateSid);
      cp(out.question, sizeof(out.question), s.question);
      cp(out.requestId, sizeof(out.requestId), s.requestId);
      const bool optionsCorrelated = stateSid[0] && s.optionSessionId[0] && strcmp(stateSid, s.optionSessionId) == 0;
      out.optionCount = optionsCorrelated ? s.optionCount : 0;
      out.attentionMode = AgentDeck::classifyAttention(true, AgentDeck::isObservedSession("", stateSid),
                                                       out.requestId[0] != '\0', optionsCorrelated, out.optionCount);
      found = true;
    }
  }
  AgentDeck::unlockState();
  return found;
}

uint32_t AgentDashboardActivity::awaitingSignature(const AwaitingItem& it) {
  uint32_t h = 2166136261u;
  h = fnvUpdate(h, it.sid, strlen(it.sid));
  h = fnvUpdate(h, it.question, strlen(it.question));
  h = fnvUpdate(h, it.requestId, strlen(it.requestId));
  h = fnvUpdate(h, &it.optionCount, sizeof(it.optionCount));
  const uint8_t mode = static_cast<uint8_t>(it.attentionMode);
  h = fnvUpdate(h, &mode, sizeof(mode));
  return h;
}

bool AgentDashboardActivity::isDismissed(const uint32_t sig) const {
  if (sig == 0) return false;
  for (int i = 0; i < kDismissedCap; i++)
    if (dismissedSigs[i] == sig) return true;
  return false;
}

bool AgentDashboardActivity::cardUsesSoftkeys(const AgentDeck::AttentionMode mode, const uint8_t optionCount) {
  // Direct button↔choice binding needs every choice on a physical key: slot 1 is
  // always Later, leaving three. Everything else (incl. >3 options) falls back to
  // the cursor grammar inside the card.
  if (mode == AgentDeck::AttentionMode::RealOptions) return optionCount <= 3;
  return mode == AgentDeck::AttentionMode::PermissionGate || mode == AgentDeck::AttentionMode::WaitingForOptions ||
         mode == AgentDeck::AttentionMode::RespondInTerminal;
}

bool AgentDashboardActivity::firstCardCandidate(AwaitingItem& out) const {
  OverviewRow rows[AgentDeckCfg::SESSIONS_CAP];
  const int n = collectOverview(rows, AgentDeckCfg::SESSIONS_CAP);
  for (int i = 0; i < n; i++) {
    if (!rows[i].awaiting) continue;
    AwaitingItem item = {};
    if (!findAwaiting(rows[i].sid, item)) continue;
    if (item.attentionMode == AgentDeck::AttentionMode::None) continue;
    if (isDismissed(awaitingSignature(item))) continue;
    out = item;
    return true;
  }
  return false;
}

void AgentDashboardActivity::serviceCard() {
  if (dashState != DashState::Connected) return;
  if (AgentDeck::OtaWs::receiving() || AgentDeck::OtaWs::flashPending()) return;  // no card takeovers mid-OTA

  if (viewMode == ViewMode::Card) {
    AwaitingItem item = {};
    if (!findAwaiting(cardSid, item) || item.attentionMode == AgentDeck::AttentionMode::None) {
      // Prompt resolved (answered here, elsewhere, or the turn moved on) — home.
      AgentLog::line("AGENT", "card resolved sid=%s", cardSid);
      cardSid[0] = '\0';
      cardSig = 0;
      viewMode = ViewMode::Overview;
      requestUpdate();
      return;
    }
    const uint32_t sig = awaitingSignature(item);
    if (sig != cardSig) {  // content changed in place (e.g. options just arrived)
      cardSig = sig;
      optionCursor = 0;
      requestUpdate();
    }
    // A managed prompt without correlated options needs focus before the daemon
    // relays them — same contract as opening Detail manually.
    if (item.attentionMode == AgentDeck::AttentionMode::WaitingForOptions && !cardFocusSent &&
        !AgentDeck::isObservedSession("", cardSid)) {
      AgentDeck::Commands::sendFocusSession(cardSid);
      cardFocusSent = true;
    }
    return;
  }

  // Auto-surface only from Overview (never hijack Detail) and only when the user
  // isn't mid-navigation.
  if (viewMode != ViewMode::Overview) return;
  if (lastUserInputMs != 0 && millis() - lastUserInputMs < kAutoSurfaceQuietMs) return;
  AwaitingItem item = {};
  if (!firstCardCandidate(item)) return;
  strncpy(cardSid, item.sid, sizeof(cardSid) - 1);
  cardSid[sizeof(cardSid) - 1] = '\0';
  cardSig = awaitingSignature(item);
  cardFocusSent = false;
  optionCursor = 0;
  viewMode = ViewMode::Card;
  AgentLog::line("AGENT", "card surfaced sid=%s mode=%u opts=%u", cardSid, (unsigned)item.attentionMode,
                 (unsigned)item.optionCount);
  requestUpdate();
}

int AgentDashboardActivity::collectOverview(OverviewRow* out, int cap) const {
  auto cp = [](char* d, size_t n, const char* s) {
    strncpy(d, s, n - 1);
    d[n - 1] = '\0';
  };
  int n = 0;
  AgentDeck::lockState();
  const auto& s = AgentDeck::g_state;
  for (uint8_t i = 0; i < s.sessionCount && n < cap; i++) {
    const auto& se = s.sessions[i];
    if (!se.alive) continue;
    OverviewRow& o = out[n++];
    cp(o.sid, sizeof(o.sid), se.id);
    cp(o.project, sizeof(o.project), se.projectName[0] ? se.projectName : "session");
    cp(o.agentType, sizeof(o.agentType), se.agentType);
    cp(o.controlMode, sizeof(o.controlMode), se.controlMode);
    cp(o.state, sizeof(o.state), se.state);
    cp(o.activity, sizeof(o.activity), se.activity);
    o.awaiting = (strncmp(se.state, "awaiting", 8) == 0);
  }
  // Observed/single-session fallback: sessions_list empty but a focused
  // state_update is live — surface it as one row so the overview isn't blank.
  if (n == 0 && cap > 0 && s.dataReceived && s.state != AgentState::DISCONNECTED) {
    OverviewRow& o = out[n++];
    cp(o.sid, sizeof(o.sid), s.sessionId[0] ? s.sessionId : s.focusedSessionId);
    cp(o.project, sizeof(o.project), s.projectName[0] ? s.projectName : "session");
    cp(o.agentType, sizeof(o.agentType), s.agentType);
    cp(o.controlMode, sizeof(o.controlMode), "");
    const bool aw = s.state == AgentState::AWAITING_PERMISSION || s.state == AgentState::AWAITING_OPTION ||
                    s.state == AgentState::AWAITING_DIFF;
    cp(o.state, sizeof(o.state), aw ? "awaiting" : (s.state == AgentState::PROCESSING ? "processing" : "idle"));
    cp(o.activity, sizeof(o.activity), s.currentTool);
    o.awaiting = aw;
  }
  AgentDeck::unlockState();
  return n;
}

uint32_t AgentDashboardActivity::bestEpochNow() {
  // NTP system clock first; else the daemon-clock estimate carried by timeline
  // events (works before SNTP lands). 0 = genuinely no clock source this boot.
  const time_t t = time(nullptr);
  if (t >= 1700000000) return (uint32_t)t;
  uint32_t epochSec = 0, epochAtMs = 0;
  AgentDeck::lockState();
  epochSec = AgentDeck::g_state.daemonEpochSec;
  epochAtMs = AgentDeck::g_state.daemonEpochAtMs;
  AgentDeck::unlockState();
  if (epochSec) return epochSec + (millis() - epochAtMs) / 1000UL;
  return 0;
}

void AgentDashboardActivity::serviceDeckPersist() {
  if (!cachedDeck) return;
  // Never compete with a firmware transfer for SD bandwidth / the WS socket.
  if (AgentDeck::OtaWs::receiving() || AgentDeck::OtaWs::flashPending()) return;
  if (lastDeckSaveMs != 0 && millis() - lastDeckSaveMs < kDeckSaveIntervalMs) return;

  // Content signature of the live deck (alive sessions only, display fields
  // only). Cheap FNV pass under the lock; the 3.5 KB snapshot copy + SD write
  // run only when this actually changed.
  auto cp = [](char* d, size_t n, const char* s) {
    strncpy(d, s, n - 1);
    d[n - 1] = '\0';
  };
  uint32_t sig = 2166136261u;
  bool dataReceived = false;
  AgentDeck::lockState();
  {
    const auto& s = AgentDeck::g_state;
    dataReceived = s.dataReceived;
    for (uint8_t i = 0; i < s.sessionCount; i++) {
      const auto& se = s.sessions[i];
      if (!se.alive) continue;
      sig = fnvUpdate(sig, se.id, strlen(se.id));
      sig = fnvUpdate(sig, se.projectName, strlen(se.projectName));
      sig = fnvUpdate(sig, se.agentType, strlen(se.agentType));
      sig = fnvUpdate(sig, se.state, strlen(se.state));
      sig = fnvUpdate(sig, se.activity, strlen(se.activity));
    }
  }
  AgentDeck::unlockState();
  if (!dataReceived) return;  // nothing real to persist yet
  if (sig == lastDeckSig) return;

  // Build the snapshot into a scratch heap buffer (never the C3 stack), write it
  // to SD without holding the state lock, then swap it in as the RAM fallback
  // under the lock (the render task reads cachedDeck through g_stateMutex).
  std::unique_ptr<AgentDeck::DeckStore::Snapshot> snap(new (std::nothrow) AgentDeck::DeckStore::Snapshot());
  if (!snap) return;
  memset(snap.get(), 0, sizeof(*snap));
  AgentDeck::lockState();
  {
    const auto& s = AgentDeck::g_state;
    for (uint8_t i = 0; i < s.sessionCount && snap->count < AgentDeckCfg::SESSIONS_CAP; i++) {
      const auto& se = s.sessions[i];
      if (!se.alive) continue;
      auto& r = snap->records[snap->count++];
      cp(r.sid, sizeof(r.sid), se.id);
      cp(r.project, sizeof(r.project), se.projectName[0] ? se.projectName : "session");
      cp(r.agentType, sizeof(r.agentType), se.agentType);
      cp(r.state, sizeof(r.state), se.state);
      cp(r.activity, sizeof(r.activity), se.activity);
      r.awaiting = strncmp(se.state, "awaiting", 8) == 0 ? 1 : 0;
      // M6 validity class — same rule as the daemon's card feed, so WS-cached
      // and pull-cached decks grey out identically offline (card_class.h).
      cp(r.actionClass, sizeof(r.actionClass), AgentDeck::classifyCardActionClass(se.requestId[0] != '\0', se.state));
    }
  }
  AgentDeck::unlockState();
  snap->savedEpoch = bestEpochNow();

  lastDeckSaveMs = millis();
  if (!AgentDeck::DeckStore::save(*snap)) return;  // SD hiccup — retry on next change
  lastDeckSig = sig;
  const unsigned savedCount = snap->count;
  AgentDeck::lockState();
  cachedDeck.swap(snap);
  AgentDeck::unlockState();
  AgentLog::line("DECK", "deck persisted: %u cards", savedCount);
}

int AgentDashboardActivity::buildRowsFromCache(OverviewRow* out, int cap) const {
  auto cp = [](char* d, size_t n, const char* s) {
    strncpy(d, s, n - 1);
    d[n - 1] = '\0';
  };
  int n = 0;
  AgentDeck::lockState();
  if (cachedDeck) {
    for (uint8_t i = 0; i < cachedDeck->count && n < cap; i++) {
      const auto& r = cachedDeck->records[i];
      OverviewRow& o = out[n++];
      cp(o.sid, sizeof(o.sid), r.sid);
      cp(o.project, sizeof(o.project), r.project);
      cp(o.agentType, sizeof(o.agentType), r.agentType);
      o.controlMode[0] = '\0';  // display-only: a cached card is never actionable
      cp(o.state, sizeof(o.state), r.state);
      // A cached live-class card (permission gate / prompt) must read as
      // unanswerable — say so instead of showing the stale prompt as pressable.
      if (AgentDeck::actionClassIsLive(r.actionClass)) {
        snprintf(o.activity, sizeof(o.activity), "Reconnect to act \xC2\xB7 %s", r.activity);
      } else {
        cp(o.activity, sizeof(o.activity), r.activity);
      }
      o.awaiting = r.awaiting != 0;
    }
  }
  AgentDeck::unlockState();
  return n;
}

void AgentDashboardActivity::handleButtons() {
  using Btn = MappedInputManager::Button;

  // Stamp any press: auto-surface must not steal the screen mid-navigation.
  if (mappedInput.wasAnyPressed()) lastUserInputMs = millis();

  // A WiFi OTA transfer rides this activity's WS socket — exiting (or any other
  // action) mid-transfer would tear it down. Swallow input until it resolves.
  if (AgentDeck::OtaWs::receiving() || AgentDeck::OtaWs::flashPending()) return;

  // Stamp Back press so release can tell short from long-hold.
  if (mappedInput.wasPressed(Btn::Back)) backPressMs = millis();

  // Not connected yet (WiFi select / discovering / connecting): the only action is
  // Back = leave the dashboard. Keep it responsive in every pre-Connected state so
  // the user is never trapped on a "searching…" screen with no way out.
  if (dashState != DashState::Connected) {
    if (mappedInput.wasReleased(Btn::Back) && backPressMs != 0) exitRequested = true;
    return;
  }

  // ── CARD: full-screen decision — one question, direct physical choices ──
  if (viewMode == ViewMode::Card) {
    AwaitingItem item = {};
    if (!findAwaiting(cardSid, item) || item.attentionMode == AgentDeck::AttentionMode::None) {
      // Resolved between service ticks; serviceCard will repaint — just don't act.
      return;
    }
    const AgentDeck::AttentionMode mode = item.attentionMode;
    const int optCount = (mode == AgentDeck::AttentionMode::PermissionGate) ? 2
                         : (mode == AgentDeck::AttentionMode::RealOptions)  ? item.optionCount
                                                                            : 0;

    // Dismiss ("Later"): remember this prompt's signature so it never re-surfaces
    // unchanged, and clear backPressMs so the release can't read as "exit" from
    // the Overview branch on the next frame.
    const auto dismissCard = [&]() {
      dismissedSigs[dismissedHead] = cardSig;
      dismissedHead = static_cast<uint8_t>((dismissedHead + 1) % kDismissedCap);
      AgentLog::line("AGENT", "card dismissed sid=%s", cardSid);
      cardSid[0] = '\0';
      cardSig = 0;
      viewMode = ViewMode::Overview;
      backPressMs = 0;
      requestUpdate();
    };
    // Open this session's Detail (timeline). The decision stays available there
    // via the inline fallback grammar. Cooldown-stamp so the mapped release of
    // the same physical press can't immediately fire an inline decision.
    const auto openDetail = [&]() {
      strncpy(selectedSid, cardSid, sizeof(selectedSid) - 1);
      selectedSid[sizeof(selectedSid) - 1] = '\0';
      optionCursor = 0;
      detailScroll = 0;
      viewMode = ViewMode::Detail;
      lastDecisionMs = millis();
      backPressMs = 0;
      swallowConfirmRelease = true;
      if (!AgentDeck::isObservedSession("", selectedSid)) AgentDeck::Commands::sendFocusSession(selectedSid);
      AgentDeck::Commands::sendQuerySessionTimeline(selectedSid);
      requestUpdate();
    };

    if (cardUsesSoftkeys(mode, item.optionCount)) {
      // Raw physical order (left→right: BTN_BACK, BTN_CONFIRM, BTN_LEFT,
      // BTN_RIGHT) — renderCard draws hint labels in the same positional order,
      // bypassing the user's logical remap, so display and input always agree.
      const int raw = mappedInput.getPressedFrontButton();
      if (raw == HalGPIO::BTN_BACK) {  // slot 1: Later
        dismissCard();
        return;
      }
      if (mode == AgentDeck::AttentionMode::PermissionGate) {
        if (raw == HalGPIO::BTN_CONFIRM) openDetail();                    // slot 2: Detail
        else if (raw == HalGPIO::BTN_LEFT) applyDecision(item, 1);        // slot 3: Deny
        else if (raw == HalGPIO::BTN_RIGHT) applyDecision(item, 0);       // slot 4: Allow
      } else if (mode == AgentDeck::AttentionMode::RealOptions) {
        int pos = -1;
        if (raw == HalGPIO::BTN_CONFIRM) pos = 0;       // slot 2: option 1
        else if (raw == HalGPIO::BTN_LEFT) pos = 1;     // slot 3: option 2
        else if (raw == HalGPIO::BTN_RIGHT) pos = 2;    // slot 4: option 3
        if (pos >= 0 && pos < optCount) applyDecision(item, pos);
      } else {  // WaitingForOptions / RespondInTerminal: read-only card
        if (raw == HalGPIO::BTN_CONFIRM) openDetail();  // slot 2: Detail
      }
      return;
    }

    // Cursor grammar (>3 options): side/front navigation moves the highlight,
    // OK selects, Back = Later. Mirrors the Detail inline decision gestures.
    if (mappedInput.wasReleased(Btn::NavNext) && optionCursor < optCount - 1) {
      optionCursor++;
      requestUpdate();
    }
    if (mappedInput.wasReleased(Btn::NavPrevious) && optionCursor > 0) {
      optionCursor--;
      requestUpdate();
    }
    if (mappedInput.wasReleased(Btn::Confirm) && optCount > 0) {
      applyDecision(item, optionCursor);
      requestUpdate();
    }
    if (mappedInput.wasReleased(Btn::Back) && backPressMs != 0) dismissCard();
    return;
  }

  // ── DETAIL: session timeline + (when awaiting) the decision options inline ──
  if (viewMode == ViewMode::Detail) {
    // Resolve the attention contract for this session. Only a requestId gate or
    // explicitly-correlated real options is actionable; observed terminal-only
    // prompts never acquire synthetic buttons.
    AwaitingItem item = {};
    const bool awaiting = findAwaiting(selectedSid, item);
    AgentDeck::AttentionMode attentionMode = AgentDeck::AttentionMode::None;
    int optCount = 0;
    if (awaiting) {
      attentionMode = item.attentionMode;
      if (attentionMode == AgentDeck::AttentionMode::PermissionGate)
        optCount = 2;  // real PreToolUse gate: Allow / Deny
      else if (attentionMode == AgentDeck::AttentionMode::RealOptions)
        optCount = item.optionCount;
    }
    if (optCount > 0) {
      if (optionCursor >= optCount) optionCursor = optCount - 1;
      if (optionCursor < 0) optionCursor = 0;
    }
    const bool atBottom = (detailScroll >= detailMaxScroll);

    // Up/Down: scroll the content; once at the bottom (options in view) the same
    // buttons move the option highlight, so it's one continuous gesture.
    if (mappedInput.wasReleased(Btn::NavNext)) {  // Down
      if (awaiting && atBottom && optionCursor < optCount - 1)
        optionCursor++;
      else if (detailScroll < detailMaxScroll)
        detailScroll++;
      requestUpdate();
    }
    if (mappedInput.wasReleased(Btn::NavPrevious)) {  // Up
      if (awaiting && atBottom && optionCursor > 0)
        optionCursor--;
      else if (detailScroll > 0)
        detailScroll--;
      requestUpdate();
    }

    // OK confirms the highlighted option once the decision is in view.
    if (swallowConfirmRelease && mappedInput.wasReleased(Btn::Confirm)) {
      // The release of the raw press that opened Detail from the Card — inert.
      swallowConfirmRelease = false;
    } else if (mappedInput.wasReleased(Btn::Confirm) && AgentDeck::attentionIsActionable(attentionMode) &&
               optCount > 0 && atBottom) {
      applyDecision(item, optionCursor);
      // Stay in Detail until the daemon confirms the state transition. This
      // avoids making a dropped/no-op command look successful on slow e-ink.
      requestUpdate();
    }

    if (mappedInput.wasReleased(Btn::Back)) {
      viewMode = ViewMode::Overview;
      requestUpdate();
    }
    return;
  }

  // ── OVERVIEW: mission-control list (home) ──
  OverviewRow rows[AgentDeckCfg::SESSIONS_CAP];
  const int n = collectOverview(rows, AgentDeckCfg::SESSIONS_CAP);
  if (overviewCursor >= n) overviewCursor = n > 0 ? n - 1 : 0;
  if (overviewCursor < 0) overviewCursor = 0;

  if (mappedInput.wasReleased(Btn::NavPrevious) && n > 1) {
    overviewCursor = (overviewCursor - 1 + n) % n;
    requestUpdate();
  }
  if (mappedInput.wasReleased(Btn::NavNext) && n > 1) {
    overviewCursor = (overviewCursor + 1) % n;
    requestUpdate();
  }

  // Confirm opens the selected session's Detail (timeline + any inline decision).
  if (mappedInput.wasReleased(Btn::Confirm) && n > 0) {
    const OverviewRow& sel = rows[overviewCursor];
    strncpy(selectedSid, sel.sid, sizeof(selectedSid) - 1);
    selectedSid[sizeof(selectedSid) - 1] = '\0';
    optionCursor = 0;
    detailScroll = 0;
    viewMode = ViewMode::Detail;
    // A managed PTY must be focused before the daemon can emit its correlated
    // state_update / prompt_options. Observed sessions cannot be focused into a
    // controllable PTY, so querying their read-only timeline is all we request.
    if (!AgentDeck::isObservedSession(sel.controlMode, selectedSid)) AgentDeck::Commands::sendFocusSession(selectedSid);
    // The live timeline_event stream is forward-only, so request this session's
    // recent history to fill Detail on open.
    AgentDeck::Commands::sendQuerySessionTimeline(selectedSid);
    requestUpdate();
  }

  // Back exits the dashboard (guard a stale release from a prior activity).
  if (mappedInput.wasReleased(Btn::Back) && backPressMs != 0) exitRequested = true;
}

bool AgentDashboardActivity::applyDecision(const AwaitingItem& it, int selectedCursor) {
  if (millis() - lastDecisionMs < kDecisionCooldownMs) return false;

  if (it.attentionMode == AgentDeck::AttentionMode::PermissionGate) {
    if (!it.requestId[0] || (selectedCursor != 0 && selectedCursor != 1)) return false;
    AgentDeck::Commands::sendPermissionDecision(it.requestId, selectedCursor == 0 ? "allow" : "deny");
    AgentLog::line("AGENT", "permission_decision=%s sid=%s req=%s", selectedCursor == 0 ? "allow" : "deny", it.sid,
                   it.requestId);
  } else if (it.attentionMode == AgentDeck::AttentionMode::RealOptions) {
    int optionIndex = -1;
    bool navigable = false;
    char action[40] = {0};
    AgentDeck::lockState();
    const auto& s = AgentDeck::g_state;
    const bool stillCorrelated = it.sid[0] && s.optionSessionId[0] && strcmp(it.sid, s.optionSessionId) == 0;
    if (stillCorrelated && selectedCursor >= 0 && selectedCursor < s.optionCount) {
      optionIndex = s.options[selectedCursor].index;
      navigable = s.navigable;
      strncpy(action, s.options[selectedCursor].action, sizeof(action) - 1);
    }
    AgentDeck::unlockState();
    if (optionIndex < 0) return false;
    if (navigable) {
      AgentDeck::Commands::sendSelectOption(it.sid, optionIndex);
      AgentLog::line("AGENT", "select_option idx=%d sid=%s", optionIndex, it.sid);
    } else {
      if (!action[0]) return false;
      AgentDeck::Commands::sendRespond(action);
      AgentLog::line("AGENT", "respond shortcut=%s sid=%s", action, it.sid);
    }
  } else {
    return false;
  }

  lastDecisionMs = millis();
  requestUpdate();
  return true;
}

int AgentDashboardActivity::loadKoreanFont() {
  // Loaded once per boot; the SdCardFont is owned by the renderer thereafter.
  static int cached = -1;  // -1 = not yet attempted
  if (cached >= 0) return cached;
  cached = 0;
  // Hidden root preferred; visible /fonts/ as a fallback (mirrors SdCardFontRegistry).
  const char* paths[] = {"/.fonts/AgentDeckKR/AgentDeckKR_12.cpfont", "/fonts/AgentDeckKR/AgentDeckKR_12.cpfont"};
  for (const char* path : paths) {
    auto* font = new (std::nothrow) SdCardFont();
    if (!font) return cached;
    if (font->load(path)) {
      const int id = 0x41444B52;  // 'ADKR' — fixed aux id, distinct from reader font
      if (renderer.getFontMap().count(id) == 0) {
        renderer.registerSdCardFont(id, font);
        EpdFontFamily fam(font->getEpdFont(0), font->getEpdFont(1), font->getEpdFont(2), font->getEpdFont(3));
        renderer.insertFont(id, fam);
        cached = id;
        AgentLog::line("AGENT", "Korean font loaded: %s", path);
        return cached;
      }
    }
    delete font;
  }
  AgentLog::line("AGENT", "Korean font not on SD (CJK falls back to reader font / box)");
  return cached;
}

int AgentDashboardActivity::fontForText(int uiFontId, const char* text) const {
  if (cjkFontId != 0 && hasCJK(text)) {
    renderer.ensureSdCardFontReady(cjkFontId, text, 0x0F);
    return cjkFontId;
  }
  return uiFontId;
}

void AgentDashboardActivity::drawBrandedHeader(const char* title, const char* subtitle) const {
  const auto& m = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();
  const Rect r{0, m.topPadding, w, m.headerHeight};
  // Pass nullptr title: Lyra's drawHeader draws the title at contentSidePadding —
  // exactly where our mark goes — and also owns the divider line (it lives inside
  // its `if (title)` block). So we let drawHeader place the battery + subtitle, then
  // lay out the mark + wordmark together on one line and redraw the divider.
  GUI.drawHeader(renderer, r, nullptr, subtitle);

  constexpr int sz = 32;  // mark width must be a multiple of 8 (see kGlyphPx note)
  const int iconY = r.y + (m.headerHeight - sz) / 2 - 2;
  renderer.drawIcon(AgentDeckMark, m.contentSidePadding, iconY, sz, sz);

  if (title) {
    const int line12 = renderer.getLineHeight(UI_12_FONT_ID);
    const int titleX = m.contentSidePadding + sz + 10;
    const int titleY = r.y + (m.headerHeight - line12) / 2;
    renderer.drawText(UI_12_FONT_ID, titleX, titleY, title, true, EpdFontFamily::BOLD);
  }
  // Divider, matching LyraTheme::drawHeader (3px rule along the header baseline).
  renderer.drawLine(r.x, r.y + r.height - 3, r.x + r.width - 1, r.y + r.height - 3, 3, true);
}

int AgentDashboardActivity::drawLimitsFooter() const {
  // Snapshot usage + the best-effort subscription summary under the lock.
  float five, seven, cxFive, cxSeven;
  bool stale;
  char fiveReset[32], sevenReset[32], cxFiveReset[32], cxSevenReset[32];
  char codexPlan[16], codexUntil[32], agPlan[24];
  AgentDeck::lockState();
  const auto& s = AgentDeck::g_state;
  five = s.fiveHourPercent;
  seven = s.sevenDayPercent;
  cxFive = s.codexFivePercent;
  cxSeven = s.codexSevenPercent;
  stale = s.usageStale;
  auto cp = [](char* d, size_t n, const char* src) {
    strncpy(d, src, n - 1);
    d[n - 1] = '\0';
  };
  cp(fiveReset, sizeof(fiveReset), s.fiveHourReset);
  cp(sevenReset, sizeof(sevenReset), s.sevenDayReset);
  cp(cxFiveReset, sizeof(cxFiveReset), s.codexFiveReset);
  cp(cxSevenReset, sizeof(cxSevenReset), s.codexSevenReset);
  cp(codexPlan, sizeof(codexPlan), s.codexPlan);
  cp(codexUntil, sizeof(codexUntil), s.codexActiveUntil);
  cp(agPlan, sizeof(agPlan), s.antigravityPlan);
  AgentDeck::unlockState();

  // Subscription line carries ChatGPT plan/expiry + Antigravity plan. Antigravity's
  // real group limits (5h/weekly %) are backend-only (never on disk) and the local
  // credit aggregate is unrelated, so only the plan name is shown — never a number.
  const bool hasChatGpt = (codexPlan[0] || codexUntil[0]);
  const int pageH = renderer.getScreenHeight();
  const bool hasClaude = (five >= 0 || seven >= 0);
  const bool hasCodex = (cxFive >= 0 || cxSeven >= 0);
  const bool codexSingleWindow = hasCodex && ((cxFive >= 0) != (cxSeven >= 0));
  const bool inlineCodexSub = codexSingleWindow && hasChatGpt;
  // When Codex only supplies 7D (current upstream shape), reuse its empty 5H
  // cell for subscription expiry instead of spending a whole extra footer row.
  const bool hasSubRow = agPlan[0] || (hasChatGpt && !inlineCodexSub);
  if (!hasClaude && !hasCodex && !hasSubRow) return pageH;  // nothing → hide footer

  const auto& m = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();
  const int pad = m.contentSidePadding;
  const int lineS = renderer.getLineHeight(SMALL_FONT_ID);
  const int rowCount = (hasClaude ? 1 : 0) + (hasCodex ? 1 : 0) + (hasSubRow ? 1 : 0);
  const int rowH = (lineS > 16 ? lineS : 16) + 6;  // hold a 16px solution icon
  const int bandH = 12 + rowCount * rowH;
  const int top = pageH - m.buttonHintsHeight - bandH;

  renderer.drawLine(pad, top, w - pad, top);  // separator above the footer
  int y = top + 12;
  constexpr int iconPx = 16;
  const int iconColW = iconPx + 8;                     // solution icon column (replaces the name label)
  const int colW = (w - pad * 2 - iconColW - 12) / 2;  // 5H | 7D columns
  const int iconDY = (lineS - iconPx) / 2;             // align icon to the text line

  auto subscriptionCell = [&](int x) {
    renderer.drawText(SMALL_FONT_ID, x, y, "SUB", true, EpdFontFamily::BOLD);
    const int labelW = renderer.getTextWidth(SMALL_FONT_ID, "SUB", EpdFontFamily::BOLD);
    char value[40] = {0};
    char date[11] = {0};
    if (codexUntil[0]) strncpy(date, codexUntil, 10);
    if (codexPlan[0] && date[0])
      snprintf(value, sizeof(value), "%s %s", codexPlan, date);
    else
      snprintf(value, sizeof(value), "%s", date[0] ? date : codexPlan);
    renderer.drawText(SMALL_FONT_ID, x + labelW + 6, y, value, true);
  };

  // One agent row: "<icon>  5H[gauge]NN% rem   7D[gauge]NN% rem".
  auto agentRow = [&](const uint8_t* icon, float a, const char* aIso, float b, const char* bIso) {
    renderer.drawIcon(icon, pad, y + iconDY, iconPx, iconPx);
    const int x0 = pad + iconColW;
    auto quota = [&](int x, const char* tag, float pct, const char* iso) {
      if (pct < 0) return;
      char rem[16];
      const bool hasRemaining = formatResetRemaining(iso, rem, sizeof(rem));
      char rt[28];
      if (hasRemaining)
        snprintf(rt, sizeof(rt), "%d%%%s %s", (int)(pct + 0.5f), stale ? "*" : "", rem);
      else
        snprintf(rt, sizeof(rt), "%d%%%s", (int)(pct + 0.5f), stale ? "*" : "");
      renderer.drawText(SMALL_FONT_ID, x, y, tag, true, EpdFontFamily::BOLD);
      const int lw = renderer.getTextWidth(SMALL_FONT_ID, tag, EpdFontFamily::BOLD);
      const int rw = renderer.getTextWidth(SMALL_FONT_ID, rt);
      const int gx = x + lw + 6;
      const int gw = colW - lw - 6 - rw - 6;
      const int gh = lineS - 4;
      if (gw > 8) {
        renderer.drawRect(gx, y + 1, gw, gh);
        const int fw = (int)((gw - 2) * (pct / 100.0f));
        if (fw > 0) renderer.fillRect(gx + 1, y + 2, fw, gh - 2);
      }
      renderer.drawText(SMALL_FONT_ID, x + colW - rw, y, rt, true);
    };
    if (a >= 0)
      quota(x0, "5H", a, aIso);
    else if (inlineCodexSub && icon == GlyphCodex16)
      subscriptionCell(x0);
    if (b >= 0)
      quota(x0 + colW + 12, "7D", b, bIso);
    else if (inlineCodexSub && icon == GlyphCodex16)
      subscriptionCell(x0 + colW + 12);
    y += rowH;
  };

  if (hasClaude) agentRow(GlyphClaude16, five, fiveReset, seven, sevenReset);
  if (hasCodex) agentRow(GlyphCodex16, cxFive, cxFiveReset, cxSeven, cxSevenReset);

  // Subscription row: [OpenAI icon] ChatGPT plan/date   [Antigravity icon] plan.
  if (hasSubRow) {
    int sx = pad;
    if (hasChatGpt && !inlineCodexSub) {
      renderer.drawIcon(GlyphCodex16, sx, y + iconDY, iconPx, iconPx);
      sx += iconColW;
      char t[48] = {0};
      int o = 0;
      if (codexPlan[0]) o += snprintf(t + o, sizeof(t) - o, "%s", codexPlan);
      if (codexUntil[0]) {
        char d[11] = {0};
        strncpy(d, codexUntil, 10);
        o += snprintf(t + o, sizeof(t) - o, "%s%s", codexPlan[0] ? " - " : "", d);
      }
      renderer.drawText(SMALL_FONT_ID, sx, y, t, true);
      sx += renderer.getTextWidth(SMALL_FONT_ID, t) + 16;
    }
    if (agPlan[0]) {
      renderer.drawIcon(GlyphAntigravity16, sx, y + iconDY, iconPx, iconPx);
      sx += iconColW;
      renderer.drawText(SMALL_FONT_ID, sx, y, renderer.truncatedText(SMALL_FONT_ID, agPlan, w - pad - sx).c_str(),
                        true);
    }
  }
  return top;
}

void AgentDashboardActivity::drawOverviewCard(const OverviewRow& row, int x, int y, int w, int h, bool selected) const {
  const int inner = 10;
  const int line10 = renderer.getLineHeight(UI_10_FONT_ID);
  const int lineS = renderer.getLineHeight(SMALL_FONT_ID);
  const auto status = AgentDeckEink::classifyStatus(row.state);
  const bool attention = status == AgentDeckEink::StatusKind::Attention;
  const bool active = attention || status == AgentDeckEink::StatusKind::Processing;

  // Paper-native hierarchy: a stable outline for every session, a second
  // outline + left rail for keyboard focus, and a filled status chip only for
  // states that need the human. No gray fills/dither, so partial refreshes stay
  // crisp on both X3 and X4.
  renderer.drawRect(x, y, w, h);
  if (selected) {
    renderer.drawRect(x + 2, y + 2, w - 4, h - 4);
    renderer.fillRect(x + 5, y + 8, 3, h - 16, true);
  }

  const uint8_t* glyph = glyphForAgent(row.agentType);
  const int glyphX = x + inner + (selected ? 5 : 0);
  const int glyphY = y + (h - kGlyphPx) / 2;
  if (glyph) renderer.drawIcon(glyph, glyphX, glyphY, kGlyphPx, kGlyphPx);

  const int textX = glyphX + kGlyphPx + 12;
  const char* badge = stateBadge(row.state);
  const int badgeTextW = renderer.getTextWidth(SMALL_FONT_ID, badge, EpdFontFamily::BOLD);
  const int badgeW = badgeTextW + 14;
  const int badgeX = x + w - inner - badgeW;
  const int titleY = row.activity[0] ? y + 12 : y + (h - line10) / 2;
  if (attention) {
    renderer.fillRect(badgeX, titleY - 3, badgeW, lineS + 6, true);
    renderer.drawText(SMALL_FONT_ID, badgeX + 7, titleY, badge, false, EpdFontFamily::BOLD);
  } else {
    // Processing is bold but not inverted; idle/offline remain quiet.
    renderer.drawText(SMALL_FONT_ID, badgeX, titleY, badge, true,
                      active ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  }

  const int projectFont = fontForText(UI_10_FONT_ID, row.project);
  const int projectMaxW = badgeX - textX - 10;
  renderer.drawText(
      projectFont, textX, titleY,
      renderer
          .truncatedText(projectFont, row.project, projectMaxW, selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR)
          .c_str(),
      true, selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);

  if (row.activity[0]) {
    const int activityFont = fontForText(SMALL_FONT_ID, row.activity);
    const int activityY = titleY + line10 + 5;
    const int lineAdvance = renderer.getLineHeight(activityFont) + 2;
    int maxLines = (y + h - inner - activityY + lineAdvance - 1) / lineAdvance;
    if (maxLines < 1) maxLines = 1;
    if (maxLines > 3) maxLines = 3;
    drawWrappedFixed(renderer, activityFont, textX, activityY, row.activity, x + w - inner - textX, maxLines,
                     lineAdvance);
  }
}

void AgentDashboardActivity::renderOverview(const OverviewRow* rows, int n, int awaitingCount, bool fromCache,
                                            uint32_t asOfEpoch) {
  const auto& m = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();
  const int line10 = renderer.getLineHeight(UI_10_FONT_ID);
  const int lineS = renderer.getLineHeight(SMALL_FONT_ID);

  bool dataReceived;
  AgentDeck::lockState();
  dataReceived = AgentDeck::g_state.dataReceived;
  AgentDeck::unlockState();

  renderer.clearScreen();
  drawBrandedHeader("AgentDeck", nullptr);
  int y = m.topPadding + m.headerHeight + m.verticalSpacing;

  // Footer first so the shared layout can reserve its exact dynamic height.
  const int footTop = drawLimitsFooter();
  const int bannerH = awaitingCount > 0 ? line10 + 22 : 0;
  const int asOfH = fromCache ? lineS + 6 : 0;  // "as of" sync-age line (M5.5)
  const int chromeH = y + lineS + 8 + asOfH + bannerH;
  const int reservedBottom = pageH - footTop;
  const AgentDeckEink::Layout layout = AgentDeckEink::makeLayout(AgentDeckEink::LayoutInput{
      (int16_t)w,
      (int16_t)pageH,
      (int16_t)chromeH,
      (int16_t)reservedBottom,
      (int16_t)(lineS + 6),
      (int16_t)lineS,
      0,
      0,
      (uint8_t)n,
      (uint8_t)(pageH > w ? 5 : 2),
  });

  // Page by card capacity. Cursor semantics stay unchanged, but X3 uses a
  // portrait single-column page while X4 automatically uses 2/3 columns.
  const int capacity = layout.capacity > 0 ? layout.capacity : 1;
  overviewTop = (overviewCursor / capacity) * capacity;
  const int firstShown = n > 0 ? overviewTop + 1 : 0;
  int lastShown = overviewTop + capacity;
  if (lastShown > n) lastShown = n;

  // Connection is a status line on the Face, never a screen of its own. An
  // in-flight firmware transfer takes over the line (bold) — the most useful
  // truth of the moment.
  char cl[96];
  bool statusBold = false;
  if (AgentDeck::OtaWs::receiving()) {
    const uint32_t total = AgentDeck::OtaWs::totalBytes();
    const unsigned pct = total ? (unsigned)((uint64_t)AgentDeck::OtaWs::receivedBytes() * 100 / total) : 0;
    snprintf(cl, sizeof(cl), "Receiving firmware update \xC2\xB7 %u%%", pct);
    statusBold = true;
  } else if (timedSleepImminent || (pullMode && pullSynced)) {
    // M6 battery cadence: this frame may be the one the panel holds through the
    // whole sleep — it must say what it is. The "as of" line carries the age.
    snprintf(cl, sizeof(cl), "Synced \xC2\xB7 sleeping until next pull \xC2\xB7 power btn wakes");
    statusBold = true;
  } else if (pullMode) {
    snprintf(cl, sizeof(cl), "Pull sync\xE2\x80\xA6 \xC2\xB7 Wi-Fi %s", localIp.c_str());
  } else {
    switch (dashState) {
    case DashState::Connected:
      if (fromCache)  // connected but first live state hasn't landed — the rows
                      // on screen are the persisted deck, so don't count them as live
        snprintf(cl, sizeof(cl), "Live \xC2\xB7 %s \xC2\xB7 syncing\xE2\x80\xA6", AgentDeck::Net::wsBridgeIp());
      else if (n > capacity)
        snprintf(cl, sizeof(cl), "Live \xC2\xB7 %s \xC2\xB7 %d-%d/%d", AgentDeck::Net::wsBridgeIp(), firstShown,
                 lastShown, n);
      else
        snprintf(cl, sizeof(cl), "Live \xC2\xB7 %s \xC2\xB7 %d", AgentDeck::Net::wsBridgeIp(), n);
      break;
    case DashState::Connecting:
      snprintf(cl, sizeof(cl), "Connecting \xC2\xB7 %s:%u\xE2\x80\xA6", AgentDeck::Net::wsBridgeIp(),
               (unsigned)AgentDeck::Net::wsBridgePort());
      break;
    case DashState::Discovering:
      if (discoveryNoticeShown)
        snprintf(cl, sizeof(cl), "AgentDeck not found \xC2\xB7 still scanning\xE2\x80\xA6");
      else
        snprintf(cl, sizeof(cl), "Searching for AgentDeck\xE2\x80\xA6 \xC2\xB7 Wi-Fi %s", localIp.c_str());
      break;
    case DashState::WifiJoining:
      snprintf(cl, sizeof(cl), "Joining Wi-Fi \"%s\"\xE2\x80\xA6", joiningSsid);
      break;
    case DashState::WifiSelection:
      snprintf(cl, sizeof(cl), "Wi-Fi setup\xE2\x80\xA6");
      break;
    }
  }
  renderer.drawText(SMALL_FONT_ID, layout.pad, y, renderer.truncatedText(SMALL_FONT_ID, cl, w - layout.pad * 2).c_str(),
                    true, statusBold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  y += lineS + 8;

  // M5.5 "as of" line: the deck on screen is the persisted snapshot, and honesty
  // requires saying when it was true. Age needs a clock (SNTP after Wi-Fi); until
  // then the line states the fact without inventing a number.
  if (fromCache) {
    char asOf[64];
    const time_t nowT = time(nullptr);
    if (asOfEpoch && nowT >= 1700000000 && (uint32_t)nowT >= asOfEpoch) {
      const uint32_t age = (uint32_t)nowT - asOfEpoch;
      if (age < 60) {
        snprintf(asOf, sizeof(asOf), "Last synced deck \xC2\xB7 just now");
      } else {
        char a[8];
        formatAge(age, a, sizeof(a));
        snprintf(asOf, sizeof(asOf), "Last synced deck \xC2\xB7 as of %s ago", a);
      }
    } else {
      snprintf(asOf, sizeof(asOf), "Last synced deck");
    }
    renderer.drawText(SMALL_FONT_ID, layout.pad, y,
                      renderer.truncatedText(SMALL_FONT_ID, asOf, w - layout.pad * 2, EpdFontFamily::BOLD).c_str(),
                      true, EpdFontFamily::BOLD);
    y += lineS + 6;
  }

  if (awaitingCount > 0) {
    const int bh = line10 + 12;
    renderer.fillRect(layout.pad, y, w - layout.pad * 2, bh, true);
    char b[48];
    snprintf(b, sizeof(b), "%d agent%s need you", awaitingCount, awaitingCount > 1 ? "s" : "");
    renderer.drawText(UI_10_FONT_ID, layout.pad + 12, y + 6, b, false, EpdFontFamily::BOLD);
  }

  if (n == 0) {
    const int emptyY = layout.cards.y + layout.cards.h / 2 - line10;
    const char* empty;
    if (dashState == DashState::Connected)
      empty = dataReceived ? "No active sessions" : "Waiting for agent state\xE2\x80\xA6";
    else if (dashState == DashState::Discovering && discoveryNoticeShown)
      empty = "Start AgentDeck on a computer on this Wi-Fi.";
    else
      empty = "Cards appear when AgentDeck connects.";
    renderer.drawText(UI_10_FONT_ID, layout.pad, emptyY, empty, true);
  } else {
    for (int i = overviewTop; i < n && i < overviewTop + capacity; i++) {
      const AgentDeckEink::Rect card = layout.card((uint8_t)(i - overviewTop));
      // Cached cards are display-only — no keyboard focus, nothing to open.
      drawOverviewCard(rows[i], card.x, card.y, card.w, card.h, !fromCache && i == overviewCursor);
    }
  }

  // Hint bar. Up/Down only meaningful with more than one row. Cached decks
  // offer no actions (handleButtons sees the live n == 0 and no-ops anyway).
  // Pull-synced rows are live data but NOT interactive (no WS session behind
  // them) — advertising "Open" there would be a lie, same honesty rule.
  const bool interactive = !fromCache && dashState == DashState::Connected;
  const auto labels = mappedInput.mapLabels("Exit", (n > 0 && interactive) ? "Open" : "",
                                            (n > 1 && interactive) ? "Up" : "", (n > 1 && interactive) ? "Down" : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void AgentDashboardActivity::renderDetail() {
  const auto& m = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();
  const int pad = m.contentSidePadding;
  const int line10 = renderer.getLineHeight(UI_10_FONT_ID);
  const int lineS = renderer.getLineHeight(SMALL_FONT_ID);

  // Snapshot the selected session + its timeline entries under one lock.
  char project[40] = {0}, agentType[16] = {0}, model[32] = {0}, state[20] = {0}, tool[40] = {0};
  char activity[AgentDeck::SESSION_ACTIVITY_CAP] = {0};
  uint32_t elapsed = 0;
  bool found = false;
  char tlText[AgentDeck::DashboardState::TIMELINE_CAP][96];
  char tlType[AgentDeck::DashboardState::TIMELINE_CAP][20];
  uint32_t tlTs[AgentDeck::DashboardState::TIMELINE_CAP];
  uint32_t epochSec = 0, epochAtMs = 0;
  int tlCount = 0;
  AgentDeck::lockState();
  const auto& s = AgentDeck::g_state;
  for (uint8_t i = 0; i < s.sessionCount; i++) {
    if (selectedSid[0] && strcmp(s.sessions[i].id, selectedSid) == 0) {
      const auto& se = s.sessions[i];
      strncpy(project, se.projectName, sizeof(project) - 1);
      strncpy(agentType, se.agentType, sizeof(agentType) - 1);
      strncpy(model, se.modelName, sizeof(model) - 1);
      strncpy(state, se.state, sizeof(state) - 1);
      strncpy(tool, se.currentTool, sizeof(tool) - 1);
      strncpy(activity, se.activity, sizeof(activity) - 1);
      elapsed = se.elapsedSec;
      found = true;
      break;
    }
  }
  if (!found) {  // observed/single-session fallback → render the focused state
    strncpy(project, s.projectName, sizeof(project) - 1);
    strncpy(agentType, s.agentType, sizeof(agentType) - 1);
    strncpy(model, s.modelName, sizeof(model) - 1);
    strncpy(tool, s.currentTool, sizeof(tool) - 1);
    strncpy(activity, s.currentTool, sizeof(activity) - 1);
    strncpy(state, agentStateLabel(s.state), sizeof(state) - 1);
    found = s.dataReceived;
  }
  // Matching timeline entries, oldest → newest (ring: head is oldest when full).
  // Unattributed rows (empty sid) are global error/scheduled signals — show them
  // in every session's Detail, matching the other dashboard surfaces.
  const int cnt = s.timelineCount;
  for (int k = 0; k < cnt; k++) {
    int idx = (s.timelineCount < AgentDeck::DashboardState::TIMELINE_CAP)
                  ? k
                  : (s.timelineHead + k) % AgentDeck::DashboardState::TIMELINE_CAP;
    const AgentDeck::TimelineItem& t = s.timeline[idx];
    if (t.sid[0] != '\0' && selectedSid[0] && strcmp(rawSid(t.sid), rawSid(selectedSid)) != 0) continue;
    if (t.text[0] == '\0') continue;
    strncpy(tlText[tlCount], t.text, sizeof(tlText[0]) - 1);
    tlText[tlCount][sizeof(tlText[0]) - 1] = '\0';
    strncpy(tlType[tlCount], t.type, sizeof(tlType[0]) - 1);
    tlType[tlCount][sizeof(tlType[0]) - 1] = '\0';
    tlTs[tlCount] = t.tsSec;
    if (++tlCount >= AgentDeck::DashboardState::TIMELINE_CAP) break;
  }
  epochSec = s.daemonEpochSec;
  epochAtMs = s.daemonEpochAtMs;
  AgentDeck::unlockState();

  // Turn grouping (mirrors the Apple/Android projection): a chat_start whose
  // turn has already completed is redundant with its chat_end/chat_response
  // row — showing both renders every turn twice. Keep only in-flight starts.
  bool tlShow[AgentDeck::DashboardState::TIMELINE_CAP];
  for (int k = 0; k < tlCount; k++) {
    tlShow[k] = true;
    if (strcmp(tlType[k], "chat_start") != 0) continue;
    for (int j = k + 1; j < tlCount; j++) {
      if (strcmp(tlType[j], "chat_end") == 0 || strcmp(tlType[j], "chat_response") == 0) {
        tlShow[k] = false;
        break;
      }
    }
  }

  // Estimated "daemon now" for per-entry ages (no RTC on this device).
  const uint32_t daemonNowSec = epochSec ? epochSec + (millis() - epochAtMs) / 1000UL : 0;

  renderer.clearScreen();
  drawBrandedHeader("Session", nullptr);
  int y = m.topPadding + m.headerHeight + m.verticalSpacing;

  if (!found) {
    renderer.drawText(UI_10_FONT_ID, pad, y, "Session ended", true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels("Back", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // Title row: glyph + project.
  const uint8_t* g = glyphForAgent(agentType);
  int textX = pad;
  if (g) {
    renderer.drawIcon(g, pad, y, kGlyphPx, kGlyphPx);
    textX = pad + kGlyphPx + 12;
  }
  renderer.drawText(
      UI_10_FONT_ID, textX, y + (kGlyphPx - line10) / 2,
      renderer.truncatedText(UI_10_FONT_ID, project[0] ? project : "session", w - textX - pad, EpdFontFamily::BOLD)
          .c_str(),
      true, EpdFontFamily::BOLD);
  y += (g ? kGlyphPx : line10) + 8;

  // Compact one-line meta: agent · model · state (· elapsed).
  char meta[140];
  int mo = snprintf(meta, sizeof(meta), "%s", agentType[0] ? agentType : "agent");
  if (model[0]) mo += snprintf(meta + mo, sizeof(meta) - mo, " \xC2\xB7 %s", model);
  if (state[0]) mo += snprintf(meta + mo, sizeof(meta) - mo, " \xC2\xB7 %s", wireStateLabel(state));
  if (elapsed > 0) {
    if (elapsed >= 60)
      mo += snprintf(meta + mo, sizeof(meta) - mo, " \xC2\xB7 %um", (unsigned)(elapsed / 60));
    else
      mo += snprintf(meta + mo, sizeof(meta) - mo, " \xC2\xB7 %us", (unsigned)elapsed);
  }
  renderer.drawText(SMALL_FONT_ID, pad, y, renderer.truncatedText(SMALL_FONT_ID, meta, w - pad * 2).c_str(), true);
  y += lineS + 6;
  if (activity[0]) {
    renderer.drawText(SMALL_FONT_ID, pad, y, "Current work", true, EpdFontFamily::BOLD);
    y += lineS + 2;
    const int activityFont = fontForText(SMALL_FONT_ID, activity);
    const int activityAdvance = renderer.getLineHeight(activityFont) + 2;
    const int activityLines =
        drawWrappedFixed(renderer, activityFont, pad, y, activity, w - pad * 2, 3, activityAdvance);
    y += activityLines * activityAdvance + 4;
  } else if (tool[0]) {
    char tl[80];
    snprintf(tl, sizeof(tl), "Now: %s", tool);
    renderer.drawText(SMALL_FONT_ID, pad, y, renderer.truncatedText(SMALL_FONT_ID, tl, w - pad * 2).c_str(), true);
    y += lineS + 6;
  }

  // Collect the attention state for this session. A real requestId gate gets
  // Allow/Deny; a managed prompt gets only its correlated daemon options; an
  // observed terminal prompt stays read-only and says so explicitly.
  AwaitingItem attention = {};
  const bool awaiting = findAwaiting(selectedSid, attention);
  const AgentDeck::AttentionMode attentionMode = awaiting ? attention.attentionMode : AgentDeck::AttentionMode::None;
  char optLabels[8][80];
  int optCount = 0;
  if (attentionMode == AgentDeck::AttentionMode::RealOptions) {
    AgentDeck::lockState();
    const auto& ds = AgentDeck::g_state;
    if (selectedSid[0] && ds.optionSessionId[0] && strcmp(selectedSid, ds.optionSessionId) == 0) {
      optCount = ds.optionCount;
      if (optCount > 8) optCount = 8;
      for (int i = 0; i < optCount; i++) {
        strncpy(optLabels[i], ds.options[i].label, sizeof(optLabels[0]) - 1);
        optLabels[i][sizeof(optLabels[0]) - 1] = '\0';
      }
    }
    AgentDeck::unlockState();
  } else if (attentionMode == AgentDeck::AttentionMode::PermissionGate) {
    optCount = 2;
    strncpy(optLabels[0], "Allow", sizeof(optLabels[0]) - 1);
    strncpy(optLabels[1], "Deny", sizeof(optLabels[0]) - 1);
    optLabels[0][sizeof(optLabels[0]) - 1] = '\0';
    optLabels[1][sizeof(optLabels[0]) - 1] = '\0';
  }

  // ── Scrollable content: timeline (oldest→newest) then the decision block ──
  renderer.drawLine(pad, y, w - pad, y);
  y += 8;
  const char* activityHeading = "Activity";
  if (AgentDeck::attentionIsActionable(attentionMode))
    activityHeading = "Activity \xC2\xB7 scroll down to decide";
  else if (attentionMode == AgentDeck::AttentionMode::RespondInTerminal)
    activityHeading = "Activity \xC2\xB7 terminal response required";
  else if (attentionMode == AgentDeck::AttentionMode::WaitingForOptions)
    activityHeading = "Activity \xC2\xB7 loading choices";
  renderer.drawText(SMALL_FONT_ID, pad, y, activityHeading, true, EpdFontFamily::BOLD);
  y += lineS + 4;

  const int listTop = y;
  const int listBottom = pageH - m.buttonHintsHeight - 8;

  // Flat line list. lineOpt: -1 normal, -2 heading (bold), >=0 = option index.
  // Reserve the worst case up front (timeline entries × 3 wrap lines + decision
  // block) — this repaints on every state change, and unreserved push_back growth
  // fragments DRAM (CLAUDE.md rule 7).
  const size_t maxLines = static_cast<size_t>(tlCount) * 3 + 12 + static_cast<size_t>(optCount);
  std::vector<std::string> lines;
  std::vector<int> lineFonts;
  std::vector<int> lineOpt;
  lines.reserve(maxLines);
  lineFonts.reserve(maxLines);
  lineOpt.reserve(maxLines);
  int tlShown = 0;
  for (int k = 0; k < tlCount; k++) {  // chronological
    if (!tlShow[k]) continue;
    // Row prefix: "[OK] 5m " — type marker (shared EINK_ICON_GLYPHS vocabulary)
    // plus the entry age from the daemon-clock estimate. Continuation lines
    // indent under the text.
    char pfx[16];
    if (daemonNowSec && tlTs[k] && daemonNowSec >= tlTs[k]) {
      char age[8];
      formatAge(daemonNowSec - tlTs[k], age, sizeof(age));
      snprintf(pfx, sizeof(pfx), "%s %s ", timelineGlyph(tlType[k]), age);
    } else {
      snprintf(pfx, sizeof(pfx), "%s ", timelineGlyph(tlType[k]));
    }
    const int fid = fontForText(SMALL_FONT_ID, tlText[k]);
    const int pfxW = renderer.getTextWidth(SMALL_FONT_ID, pfx);
    auto wrapped = renderer.wrappedText(fid, tlText[k], w - pad * 2 - pfxW, 3);
    for (size_t li = 0; li < wrapped.size(); li++) {
      lines.push_back((li == 0 ? std::string(pfx) : std::string("   ")) + wrapped[li]);
      lineFonts.push_back(fid);
      lineOpt.push_back(-1);
    }
    tlShown++;
  }
  if (tlShown == 0) {
    lines.push_back("Detailed events will appear as work progresses.");
    lineFonts.push_back(SMALL_FONT_ID);
    lineOpt.push_back(-1);
  }
  if (awaiting) {
    lines.push_back("");
    lineFonts.push_back(SMALL_FONT_ID);
    lineOpt.push_back(-1);
    lines.push_back(AgentDeck::attentionIsActionable(attentionMode) ? "Needs your decision:" : "Needs your attention:");
    lineFonts.push_back(SMALL_FONT_ID);
    lineOpt.push_back(-2);
    if (attention.question[0]) {
      const int qf = fontForText(SMALL_FONT_ID, attention.question);
      auto qlines = renderer.wrappedText(qf, attention.question, w - pad * 2, 4);
      for (auto& ql : qlines) {
        lines.push_back(ql);
        lineFonts.push_back(qf);
        lineOpt.push_back(-1);
      }
    }
    if (attentionMode == AgentDeck::AttentionMode::RespondInTerminal) {
      lines.push_back("Respond in the agent terminal.");
      lineFonts.push_back(SMALL_FONT_ID);
      lineOpt.push_back(-2);
      lines.push_back("This observed session did not expose its choices remotely.");
      lineFonts.push_back(SMALL_FONT_ID);
      lineOpt.push_back(-1);
    } else if (attentionMode == AgentDeck::AttentionMode::WaitingForOptions) {
      lines.push_back("Loading choices from the managed session...");
      lineFonts.push_back(SMALL_FONT_ID);
      lineOpt.push_back(-1);
    }
    for (int i = 0; i < optCount; i++) {
      lines.push_back(optLabels[i]);
      lineFonts.push_back(fontForText(SMALL_FONT_ID, optLabels[i]));
      lineOpt.push_back(i);
    }
  }

  const int visibleLines = (listBottom - listTop) / lineS;
  detailMaxScroll = (int)lines.size() - visibleLines;
  if (detailMaxScroll < 0) detailMaxScroll = 0;
  if (detailScroll > detailMaxScroll) detailScroll = detailMaxScroll;
  if (detailScroll < 0) detailScroll = 0;

  int ly = listTop;
  for (int i = detailScroll; i < (int)lines.size(); i++) {
    const int lh = renderer.getLineHeight(lineFonts[i]);
    if (ly + lh > listBottom) break;
    const bool isOpt = lineOpt[i] >= 0;
    const bool sel = isOpt && lineOpt[i] == optionCursor;
    const auto style = (lineOpt[i] == -2 || sel) ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    if (sel) {  // highlighted option: inverted bar + caret
      renderer.fillRect(pad - 4, ly - 1, w - pad * 2 + 8, lh, true);
      char row[100];
      snprintf(row, sizeof(row), "\xE2\x96\xB6 %s", lines[i].c_str());
      renderer.drawText(lineFonts[i], pad, ly, row, false, style);
    } else if (isOpt) {
      char row[100];
      snprintf(row, sizeof(row), "  %s", lines[i].c_str());
      renderer.drawText(lineFonts[i], pad, ly, row, true, style);
    } else {
      renderer.drawText(lineFonts[i], pad, ly, lines[i].c_str(), true, style);
    }
    ly += lh;
  }

  // Hint bar. OK selects the highlighted option only once scrolled to the
  // decision (atBottom); the heading nudges the user to scroll down to it.
  const bool atBottom = detailScroll >= detailMaxScroll;
  const auto labels = mappedInput.mapLabels(
      "Back", (AgentDeck::attentionIsActionable(attentionMode) && optCount > 0 && atBottom) ? "Select" : "", "Up",
      "Down");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void AgentDashboardActivity::renderCard() {
  const auto& m = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();
  const int pad = m.contentSidePadding;
  const int lineS = renderer.getLineHeight(SMALL_FONT_ID);

  AwaitingItem item = {};
  const bool present = findAwaiting(cardSid, item) && item.attentionMode != AgentDeck::AttentionMode::None;
  const AgentDeck::AttentionMode mode = present ? item.attentionMode : AgentDeck::AttentionMode::None;

  // Session meta + correlated option labels under one lock.
  char project[40] = {0}, agentType[16] = {0};
  char activityLine[AgentDeck::SESSION_ACTIVITY_CAP] = {0};
  char optLabels[8][80];
  bool optRecommended[8] = {false};
  int optCount = 0;
  AgentDeck::lockState();
  {
    const auto& s = AgentDeck::g_state;
    for (uint8_t i = 0; i < s.sessionCount; i++) {
      if (cardSid[0] && strcmp(s.sessions[i].id, cardSid) == 0) {
        strncpy(project, s.sessions[i].projectName, sizeof(project) - 1);
        strncpy(agentType, s.sessions[i].agentType, sizeof(agentType) - 1);
        strncpy(activityLine, s.sessions[i].activity, sizeof(activityLine) - 1);
        break;
      }
    }
    if (!project[0]) strncpy(project, s.projectName, sizeof(project) - 1);
    if (!agentType[0]) strncpy(agentType, s.agentType, sizeof(agentType) - 1);
    if (mode == AgentDeck::AttentionMode::RealOptions && s.optionSessionId[0] &&
        strcmp(s.optionSessionId, cardSid) == 0) {
      optCount = s.optionCount > 8 ? 8 : s.optionCount;
      for (int i = 0; i < optCount; i++) {
        strncpy(optLabels[i], s.options[i].label, sizeof(optLabels[0]) - 1);
        optLabels[i][sizeof(optLabels[0]) - 1] = '\0';
        optRecommended[i] = s.options[i].recommended;
      }
    }
  }
  AgentDeck::unlockState();
  if (mode == AgentDeck::AttentionMode::PermissionGate) {
    // applyDecision cursor order: 0 = Allow, 1 = Deny.
    optCount = 2;
    snprintf(optLabels[0], sizeof(optLabels[0]), "Allow");
    snprintf(optLabels[1], sizeof(optLabels[1]), "Deny");
    optRecommended[0] = optRecommended[1] = false;
  }

  renderer.clearScreen();
  drawBrandedHeader("Decision", nullptr);
  int y = m.topPadding + m.headerHeight + m.verticalSpacing;

  if (!present) {  // resolved between service ticks; serviceCard flips home next tick
    renderer.drawText(UI_10_FONT_ID, pad, y, "Resolved", true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  // Title row: glyph + project.
  const int line10 = renderer.getLineHeight(UI_10_FONT_ID);
  const uint8_t* g = glyphForAgent(agentType);
  int textX = pad;
  if (g) {
    renderer.drawIcon(g, pad, y, kGlyphPx, kGlyphPx);
    textX = pad + kGlyphPx + 12;
  }
  renderer.drawText(
      UI_10_FONT_ID, textX, y + (kGlyphPx - line10) / 2,
      renderer.truncatedText(UI_10_FONT_ID, project[0] ? project : "session", w - textX - pad, EpdFontFamily::BOLD)
          .c_str(),
      true, EpdFontFamily::BOLD);
  y += (g ? kGlyphPx : line10) + 6;

  const char* metaTail = AgentDeck::attentionIsActionable(mode) ? "needs your decision" : "needs your attention";
  char meta[80];
  snprintf(meta, sizeof(meta), "%s \xC2\xB7 %s", agentType[0] ? agentType : "agent", metaTail);
  renderer.drawText(SMALL_FONT_ID, pad, y, renderer.truncatedText(SMALL_FONT_ID, meta, w - pad * 2).c_str(), true);
  y += lineS + 6;
  renderer.drawLine(pad, y, w - pad, y);
  y += 10;

  const bool softkeys = cardUsesSoftkeys(mode, item.optionCount);

  // ── Options block: measured first so the question knows its floor ──
  // Softkey grammar rows are "[n] label" where n is the physical button slot
  // (2..4; slot 1 is Later). Cursor grammar rows are "▶ label" with highlight.
  const int optFontBase = UI_10_FONT_ID;
  int rowFonts[8];
  int rowsH = 0;
  for (int i = 0; i < optCount; i++) {
    rowFonts[i] = fontForText(optFontBase, optLabels[i]);
    rowsH += renderer.getLineHeight(rowFonts[i]) + 6;
  }
  int noteLines = 0;  // read-only card note (WaitingForOptions / RespondInTerminal)
  const char* note = nullptr;
  if (mode == AgentDeck::AttentionMode::WaitingForOptions) {
    note = "Loading choices from the managed session...";
    noteLines = 1;
  } else if (mode == AgentDeck::AttentionMode::RespondInTerminal) {
    note = "Respond in the agent terminal \xE2\x80\x94 this observed session did not expose its choices remotely.";
    noteLines = 2;
  }
  const int hintTop = pageH - m.buttonHintsHeight - 6;
  const int optionsTop = hintTop - rowsH - noteLines * (lineS + 2) - 4;

  // ── Question — the card's center. Bigger face, CJK-aware, floor-clamped ──
  const char* q = item.question[0] ? item.question
                  : (mode == AgentDeck::AttentionMode::PermissionGate ? "Approve this tool call?" : "Your turn.");
  const int qFont = fontForText(UI_12_FONT_ID, q);
  const int qAdvance = renderer.getLineHeight(qFont) + 4;
  {
    auto qLines = renderer.wrappedText(qFont, q, w - pad * 2, 6);
    for (auto& ql : qLines) {
      if (y + qAdvance > optionsTop - lineS - 8) break;  // keep room for context
      renderer.drawText(qFont, pad, y, ql.c_str(), true, EpdFontFamily::BOLD);
      y += qAdvance;
    }
  }
  y += 4;
  // Context: what the agent was doing (one line, quiet).
  if (activityLine[0] && y + lineS <= optionsTop - 4) {
    const int aFont = fontForText(SMALL_FONT_ID, activityLine);
    renderer.drawText(aFont, pad, y, renderer.truncatedText(aFont, activityLine, w - pad * 2).c_str(), true);
  }

  // ── Draw the options block bottom-anchored ──
  y = optionsTop;
  if (note) {
    auto nLines = renderer.wrappedText(SMALL_FONT_ID, note, w - pad * 2, noteLines);
    for (auto& nl : nLines) {
      renderer.drawText(SMALL_FONT_ID, pad, y, nl.c_str(), true);
      y += lineS + 2;
    }
  }
  for (int i = 0; i < optCount; i++) {
    const int lh = renderer.getLineHeight(rowFonts[i]);
    char row[96];
    if (softkeys) {
      // Physical slot numbers: PermissionGate puts Deny on 3, Allow on 4;
      // options map in order onto 2..4. Keep in lockstep with handleButtons.
      const int keycap = (mode == AgentDeck::AttentionMode::PermissionGate) ? (i == 0 ? 4 : 3) : (2 + i);
      snprintf(row, sizeof(row), "[%d] %s", keycap, optLabels[i]);
      renderer.drawText(rowFonts[i], pad, y, renderer.truncatedText(rowFonts[i], row, w - pad * 2).c_str(), true,
                        optRecommended[i] ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    } else {
      const bool sel = (i == optionCursor);
      if (sel) {
        renderer.fillRect(pad - 4, y - 1, w - pad * 2 + 8, lh + 2, true);
        snprintf(row, sizeof(row), "\xE2\x96\xB6 %s", optLabels[i]);
        renderer.drawText(rowFonts[i], pad, y, renderer.truncatedText(rowFonts[i], row, w - pad * 2).c_str(), false,
                          EpdFontFamily::BOLD);
      } else {
        snprintf(row, sizeof(row), "  %s", optLabels[i]);
        renderer.drawText(rowFonts[i], pad, y, renderer.truncatedText(rowFonts[i], row, w - pad * 2).c_str(), true,
                          optRecommended[i] ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
      }
    }
    y += lh + 6;
  }

  // ── Hint bar ──
  if (softkeys) {
    // Positional labels matching getPressedFrontButton's raw physical order —
    // deliberately NOT mapLabels(), which applies the user's logical remap.
    char h2[24] = {0}, h3[24] = {0}, h4[24] = {0};
    const auto hintFor = [&](char* out, size_t cap, const char* label, int keycap) {
      if (!label || !label[0]) {
        out[0] = '\0';
        return;
      }
      if (!hasCJK(label)) {
        snprintf(out, cap, "%s", renderer.truncatedText(UI_10_FONT_ID, label, 96).c_str());
      } else {
        snprintf(out, cap, "%d", keycap);  // body row carries the CJK label
      }
    };
    if (mode == AgentDeck::AttentionMode::PermissionGate) {
      GUI.drawButtonHints(renderer, "Later", "Detail", "Deny", "Allow");
    } else if (mode == AgentDeck::AttentionMode::RealOptions) {
      hintFor(h2, sizeof(h2), optCount > 0 ? optLabels[0] : "", 2);
      hintFor(h3, sizeof(h3), optCount > 1 ? optLabels[1] : "", 3);
      hintFor(h4, sizeof(h4), optCount > 2 ? optLabels[2] : "", 4);
      GUI.drawButtonHints(renderer, "Later", h2, h3, h4);
    } else {
      GUI.drawButtonHints(renderer, "Later", "Detail", "", "");
    }
  } else {
    const auto labels = mappedInput.mapLabels("Later", "Select", "Up", "Down");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer();
}

void AgentDashboardActivity::render(RenderLock&&) {
  // Blocking OTA flash notice: painted once via requestUpdateAndWait() right
  // before the raw-partition write starts, so the panel holds this frame for
  // the ~1 minute of flashing and through the restart.
  if (otaFlashNotice) {
    const auto& mm = UITheme::getInstance().getMetrics();
    const int pad = mm.contentSidePadding;
    renderer.clearScreen();
    drawBrandedHeader("Firmware Update", nullptr);
    int y = mm.topPadding + mm.headerHeight + mm.verticalSpacing * 2;
    renderer.drawText(UI_12_FONT_ID, pad, y, "Installing update\xE2\x80\xA6", true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_12_FONT_ID) + 10;
    renderer.drawText(UI_10_FONT_ID, pad, y, "Do not power off.", true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_10_FONT_ID) + 6;
    renderer.drawText(SMALL_FONT_ID, pad, y, "The device restarts automatically when done.", true);
    renderer.displayBuffer();
    return;
  }

  // Card (decision) / Detail (timeline) exist only while Connected.
  if (dashState == DashState::Connected) {
    if (viewMode == ViewMode::Card) {
      renderCard();
      return;
    }
    if (viewMode == ViewMode::Detail) {
      renderDetail();
      return;
    }
  }

  // ── Face: content-first shell in EVERY connection state. The Face renders
  // whatever is known (or an honest empty state); joining/discovering/connecting
  // progress is a status line inside renderOverview, never a screen that
  // replaces the content. ──
  OverviewRow rows[AgentDeckCfg::SESSIONS_CAP];
  int n = collectOverview(rows, AgentDeckCfg::SESSIONS_CAP);

  // M5.5: no live data yet (boot / daemon lost) → render the persisted deck.
  // dataReceived is the chokepoint: once any live state arrives, live wins even
  // when it says "no sessions" — the cache must never mask fresher truth.
  bool fromCache = false;
  uint32_t asOfEpoch = 0;
  if (n == 0) {
    bool dataReceived;
    AgentDeck::lockState();
    dataReceived = AgentDeck::g_state.dataReceived;
    asOfEpoch = cachedDeck ? cachedDeck->savedEpoch : 0;
    AgentDeck::unlockState();
    if (!dataReceived) {
      n = buildRowsFromCache(rows, AgentDeckCfg::SESSIONS_CAP);
      fromCache = n > 0;
    }
  }

  int awaiting = 0;
  if (!fromCache) {
    // Live only: a cached "awaiting" is a snapshot of the past, and the banner
    // is a call to action the user cannot take offline.
    for (int i = 0; i < n; i++)
      if (rows[i].awaiting) awaiting++;
  }
  renderOverview(rows, n, awaiting, fromCache, fromCache ? asOfEpoch : 0);
}
