#include "agent_commands.h"

#include <cstdio>
#include <cstring>

#include "ws_client.h"

namespace AgentDeck {
namespace Commands {

bool buildPermissionDecision(char* out, size_t cap, const char* requestId, const char* decision) {
  if (!out || cap == 0) return false;
  if (!requestId || !requestId[0] || !decision || !decision[0]) {
    out[0] = '\0';
    return false;
  }
  int n = snprintf(out, cap, "{\"type\":\"permission_decision\",\"requestId\":\"%s\",\"decision\":\"%s\"}", requestId,
                   decision);
  return n > 0 && static_cast<size_t>(n) < cap;
}

bool buildSelectOption(char* out, size_t cap, const char* sid, int index) {
  if (!out || cap == 0) return false;
  int n;
  if (sid && sid[0])
    n = snprintf(out, cap, "{\"type\":\"select_option\",\"index\":%d,\"sessionId\":\"%s\"}", index, sid);
  else
    n = snprintf(out, cap, "{\"type\":\"select_option\",\"index\":%d}", index);
  return n > 0 && static_cast<size_t>(n) < cap;
}

bool buildRespond(char* out, size_t cap, const char* value) {
  if (!out || cap == 0) return false;
  if (!value || !value[0]) {
    out[0] = '\0';
    return false;
  }
  // Shortcuts originate in PromptOption.shortcut. Reject anything requiring
  // JSON escaping rather than allocating a temporary encoded string.
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p; p++) {
    if (*p < 0x20 || *p == '"' || *p == '\\') {
      out[0] = '\0';
      return false;
    }
  }
  const int n = snprintf(out, cap, "{\"type\":\"respond\",\"value\":\"%s\"}", value);
  return n > 0 && static_cast<size_t>(n) < cap;
}

bool buildFocusSession(char* out, size_t cap, const char* sid) {
  if (!out || cap == 0) return false;
  if (!sid || !sid[0]) {
    out[0] = '\0';
    return false;
  }
  const int n = snprintf(out, cap, "{\"type\":\"focus_session\",\"sessionId\":\"%s\"}", sid);
  return n > 0 && static_cast<size_t>(n) < cap;
}

bool buildSessionEscape(char* out, size_t cap, const char* sid) {
  if (!out || cap == 0) return false;
  if (!sid || !sid[0]) {
    out[0] = '\0';
    return false;
  }
  int n =
      snprintf(out, cap, "{\"type\":\"session_command\",\"sessionId\":\"%s\",\"command\":{\"type\":\"escape\"}}", sid);
  return n > 0 && static_cast<size_t>(n) < cap;
}

bool buildQuerySessionTimeline(char* out, size_t cap, const char* sid) {
  if (!out || cap == 0) return false;
  if (!sid || !sid[0]) {
    out[0] = '\0';
    return false;
  }
  int n = snprintf(out, cap, "{\"type\":\"query_session_timeline\",\"sessionId\":\"%s\"}", sid);
  return n > 0 && static_cast<size_t>(n) < cap;
}

void sendPermissionDecision(const char* requestId, const char* decision) {
  char buf[160];
  if (buildPermissionDecision(buf, sizeof(buf), requestId, decision)) Net::queueOutbound(buf);
}

void sendSelectOption(const char* sid, int index) {
  char buf[96];
  if (buildSelectOption(buf, sizeof(buf), sid, index)) Net::queueOutbound(buf);
}

void sendRespond(const char* value) {
  char buf[96];
  if (buildRespond(buf, sizeof(buf), value)) Net::queueOutbound(buf);
}

void sendFocusSession(const char* sid) {
  char buf[128];
  if (buildFocusSession(buf, sizeof(buf), sid)) Net::queueOutbound(buf);
}

void sendQuerySessionTimeline(const char* sid) {
  char buf[160];
  if (buildQuerySessionTimeline(buf, sizeof(buf), sid)) Net::queueOutbound(buf);
}

void sendSessionEscape(const char* sid) {
  char buf[160];
  if (buildSessionEscape(buf, sizeof(buf), sid)) Net::queueOutbound(buf);
}

void sendApprove(const char* requestId, const char* sid, bool approve) {
  (void)sid;
  if (requestId && requestId[0]) {
    sendPermissionDecision(requestId, approve ? "allow" : "deny");
  }
}

}  // namespace Commands
}  // namespace AgentDeck
