#include "mdns_discovery.h"

#include <ESPmDNS.h>
#include <mdns.h>

#include <cstring>

#include "agent/AgentLog.h"
#include "agentdeck_config.h"

namespace AgentDeck {
namespace Net {

namespace {
BridgeInfo discovered;
uint32_t lastQueryMs = 0;
// In-flight async PTR search, or nullptr. ESPmDNS::queryService blocks the loop
// task for a hardcoded 3000ms (mdns_query_ptr(srv, prt, 3000, 20)), which starves
// button input every MDNS_QUERY_INTERVAL_MS while Discovering. The raw IDF async
// API lets the search run in the mDNS task while loop() keeps polling buttons.
mdns_search_once_t* activeSearch = nullptr;
// How long a single async search stays active collecting answers. Matches the old
// blocking behaviour's discovery quality without the loop stall.
constexpr uint32_t SEARCH_WINDOW_MS = 3000;
constexpr size_t SEARCH_MAX_RESULTS = 20;

// Selection priority (robust to a dual-homed host + a duplicate fallback-port
// daemon, both of which the user's network exhibits):
//   1. agent=daemon on the CANONICAL port (9120). A daemon on a fallback port
//      (9121+) is usually a duplicate that failed to demote to client — prefer
//      the real one and ignore the duplicate so we don't flap between them.
//   2. any agent=daemon.
//   3. first service with a port.
bool selectBridge(mdns_result_t* results) {
  discovered = {};
  mdns_result_t* first = nullptr;
  mdns_result_t* daemon = nullptr;
  mdns_result_t* daemonCanonical = nullptr;
  for (mdns_result_t* r = results; r != nullptr; r = r->next) {
    if (r->port == 0) continue;
    if (!first) first = r;
    bool isDaemon = false;
    for (size_t k = 0; k < r->txt_count; k++) {
      if (r->txt[k].key && strcmp(r->txt[k].key, "agent") == 0 && r->txt[k].value &&
          strcmp(r->txt[k].value, "daemon") == 0) {
        isDaemon = true;
        break;
      }
    }
    if (isDaemon) {
      if (!daemon) daemon = r;
      if (r->port == AgentDeckCfg::BRIDGE_DEFAULT_PORT && !daemonCanonical) daemonCanonical = r;
    }
  }

  mdns_result_t* sel = daemonCanonical ? daemonCanonical : (daemon ? daemon : first);
  if (!sel) return false;

  discovered.endpoints.port = sel->port;
  discovered.found = true;
  discovered.token[0] = '\0';
  discovered.project[0] = '\0';
  discovered.agent[0] = '\0';

  // Parse TXT, including the canonical `ip` the daemon publishes (its single
  // default-route address).
  char txtIp[16] = {0};
  for (size_t k = 0; k < sel->txt_count; k++) {
    const char* key = sel->txt[k].key;
    const char* val = sel->txt[k].value;
    if (!key || !val) continue;
    if (strcmp(key, "token") == 0) {
      strncpy(discovered.token, val, sizeof(discovered.token) - 1);
      discovered.token[sizeof(discovered.token) - 1] = '\0';
    } else if (strcmp(key, "project") == 0) {
      strncpy(discovered.project, val, sizeof(discovered.project) - 1);
      discovered.project[sizeof(discovered.project) - 1] = '\0';
    } else if (strcmp(key, "agent") == 0) {
      strncpy(discovered.agent, val, sizeof(discovered.agent) - 1);
      discovered.agent[sizeof(discovered.agent) - 1] = '\0';
    } else if (strcmp(key, "ip") == 0) {
      strncpy(txtIp, val, sizeof(txtIp) - 1);
      txtIp[sizeof(txtIp) - 1] = '\0';
    }
  }

  // TXT `ip` remains the first hint for old/single-homed daemons, but it is no
  // longer authoritative. Bonjour's SRV target can carry one A record per
  // active interface; preserving all of them lets the HTTP layer prove which
  // return path actually works instead of pinning the board to the default
  // route selected by the Mac.
  if (txtIp[0]) endpointCandidateAdd(discovered.endpoints, txtIp);
  for (mdns_ip_addr_t* a = sel->addr; a != nullptr; a = a->next) {
    if (a->addr.type == ESP_IPADDR_TYPE_V4) {
      const uint32_t v = a->addr.u_addr.ip4.addr;
      char ip[16];
      snprintf(ip, sizeof(ip), "%u.%u.%u.%u", (unsigned)(v & 0xFF), (unsigned)((v >> 8) & 0xFF),
               (unsigned)((v >> 16) & 0xFF), (unsigned)((v >> 24) & 0xFF));
      endpointCandidateAdd(discovered.endpoints, ip);
    }
  }
  if (!discovered.endpoints.count) return false;

  AgentLog::line("MDNS", "found bridge %s:%u candidates=%u agent=%s project=%s", discovered.primaryIp(),
                 (unsigned)discovered.endpoints.port, (unsigned)discovered.endpoints.count, discovered.agent,
                 discovered.project);
  return true;
}
}  // namespace

bool mdnsInit(const char* hostName) {
  const char* safeHostName = (hostName && hostName[0]) ? hostName : "agentdeck-xteink";
  if (!MDNS.begin(safeHostName)) {
    AgentLog::line("MDNS", "responder failed to start");
    return false;
  }
  AgentLog::line("MDNS", "host=%s browsing for %s%s", safeHostName, AgentDeckCfg::MDNS_SERVICE,
                 AgentDeckCfg::MDNS_PROTO);
  discovered = {};
  return true;
}

bool mdnsPoll(BridgeInfo& out) {
  // Harvest an in-flight search without blocking (0ms wait). Returns false while
  // the mDNS task is still collecting answers inside its SEARCH_WINDOW_MS.
  if (activeSearch) {
    mdns_result_t* results = nullptr;
    if (!mdns_query_async_get_results(activeSearch, 0, &results, nullptr)) {
      return false;
    }
    mdns_query_async_delete(activeSearch);
    activeSearch = nullptr;
    const bool found = selectBridge(results);
    if (results) mdns_query_results_free(results);
    if (found) {
      out = discovered;
      return true;
    }
    return false;
  }

  const uint32_t now = millis();
  if (now - lastQueryMs < AgentDeckCfg::MDNS_QUERY_INTERVAL_MS) return false;
  lastQueryMs = now;

  // Config strings already carry the leading underscore (_agentdeck/_tcp) the raw
  // IDF API expects; the ESPmDNS wrapper would have prepended it.
  activeSearch = mdns_query_async_new(nullptr, AgentDeckCfg::MDNS_SERVICE, AgentDeckCfg::MDNS_PROTO, MDNS_TYPE_PTR,
                                      SEARCH_WINDOW_MS, SEARCH_MAX_RESULTS, nullptr);
  if (!activeSearch) {
    AgentLog::line("MDNS", "async query start failed");
  }
  return false;
}

void mdnsRefresh() {
  lastQueryMs = 0;
  // Leave any in-flight search running; its results are harvested (and superseded
  // by a fresh query) on the next polls.
}

void mdnsStop() {
  if (activeSearch) {
    // mdns_query_async_delete requires a finished search; a still-running one is
    // abandoned here. The only caller (dashboard onExit) reboots via silentRestart
    // right after MDNS.end(), so the object is reclaimed either way.
    mdns_result_t* results = nullptr;
    if (mdns_query_async_get_results(activeSearch, 0, &results, nullptr)) {
      if (results) mdns_query_results_free(results);
      mdns_query_async_delete(activeSearch);
    }
    activeSearch = nullptr;
  }
}

}  // namespace Net
}  // namespace AgentDeck
