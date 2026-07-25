#pragma once
//
// agent_commands.h — outbound command builders ported from AgentDeck
// esp32/src/ui/widgets/hud_bar.cpp (the hudSend* helpers, lines ~146-183).
//
// Ported now (M2) so M3 can wire them to the physical buttons without further
// porting. They are split into pure builders (buildX → fills a caller buffer,
// unit-testable, no transport) and queue wrappers (sendX → enqueue via the
// thread-safe ws_client outbox). No buttons call these in M2.
//
#include <cstddef>

namespace AgentDeck {
namespace Commands {

// ── pure builders ──────────────────────────────────────────────────────────
// Each returns true on success (buffer written + NUL-terminated), false if the
// required ids are missing or the buffer is too small.

// {"type":"permission_decision","requestId":"<id>","decision":"<allow|deny>"}
bool buildPermissionDecision(char* out, size_t cap, const char* requestId, const char* decision);

// {"type":"select_option","index":<i>[,"sessionId":"<sid>"]}
bool buildSelectOption(char* out, size_t cap, const char* sid, int index);

// {"type":"respond","value":"<shortcut>"} for non-navigable prompts.
bool buildRespond(char* out, size_t cap, const char* value);

// {"type":"focus_session","sessionId":"<sid>"} — required before a
// managed session's state_update/options can be safely correlated.
bool buildFocusSession(char* out, size_t cap, const char* sid);

// {"type":"session_command","sessionId":"<sid>","command":{"type":"escape"}}
bool buildSessionEscape(char* out, size_t cap, const char* sid);

// {"type":"query_session_timeline","sessionId":"<sid>"} — request a session's
// recent timeline so Detail can fill on connect (daemon replies with a
// timeline_history scoped to this session).
bool buildQuerySessionTimeline(char* out, size_t cap, const char* sid);

// ── queue wrappers (enqueue on the ws_client outbox) ───────────────────────
void sendPermissionDecision(const char* requestId, const char* decision);
void sendSelectOption(const char* sid, int index);
void sendRespond(const char* value);
void sendFocusSession(const char* sid);
void sendSessionEscape(const char* sid);
void sendQuerySessionTimeline(const char* sid);

// Legacy compatibility wrapper. It is intentionally actionable ONLY when a
// real requestId exists; missing options/requestId must never fabricate a
// select_option or turn "Deny" into a delayed session escape.
void sendApprove(const char* requestId, const char* sid, bool approve);

}  // namespace Commands
}  // namespace AgentDeck
