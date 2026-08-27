#pragma once
//
// mdns_discovery.h — port of AgentDeck esp32/src/net/mdns_discovery.{h,cpp}.
//
// Browses for the AgentDeck daemon over mDNS (_agentdeck._tcp) and returns its
// ip/port/token from the TXT records. Polled cooperatively from the dashboard
// activity loop (no network task).
//
#include <cstdint>

#include "endpoint_candidates.h"

namespace AgentDeck {
namespace Net {

struct BridgeInfo {
  EndpointCandidates endpoints;
  char token[40];
  char project[40];
  char agent[16];
  bool found;

  const char* primaryIp() const { return endpoints.count ? endpoints.ips[0] : ""; }
};

// Start the mDNS responder + begin browsing. Returns false if responder failed.
bool mdnsInit(const char* hostName);

// Non-blocking poll. Returns true (and fills out) when a bridge is discovered.
bool mdnsPoll(BridgeInfo& out);

// Force the next poll to query immediately.
void mdnsRefresh();

// Best-effort cleanup of an in-flight async search. Call before MDNS.end().
void mdnsStop();

}  // namespace Net
}  // namespace AgentDeck
