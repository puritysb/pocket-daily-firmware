#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace AgentDeck {
namespace Net {

// A Bonjour service host may legitimately have more than one LAN address
// (Ethernet + Wi-Fi is the common case). Keep the set fixed-size so discovery,
// persistence, and pull sync never allocate on the no-PSRAM X3.
constexpr uint8_t ENDPOINT_CANDIDATE_CAP = 4;

struct EndpointCandidates {
  char ips[ENDPOINT_CANDIDATE_CAP][16] = {};
  uint8_t count = 0;
  uint16_t port = 0;
};

inline bool endpointCandidateAdd(EndpointCandidates& endpoints, const char* ip) {
  if (!ip || !ip[0] || strlen(ip) >= sizeof(endpoints.ips[0])) return false;
  for (uint8_t i = 0; i < endpoints.count; i++) {
    if (strcmp(endpoints.ips[i], ip) == 0) return true;
  }
  if (endpoints.count >= ENDPOINT_CANDIDATE_CAP) return false;
  strcpy(endpoints.ips[endpoints.count++], ip);
  return true;
}

// Make a proven address the fast path while retaining every discovered
// alternative. If it is new and the set is full, evict only the least-preferred
// tail entry.
inline bool endpointCandidatePromote(EndpointCandidates& endpoints, const char* ip) {
  if (!ip || !ip[0] || strlen(ip) >= sizeof(endpoints.ips[0])) return false;
  uint8_t found = endpoints.count;
  for (uint8_t i = 0; i < endpoints.count; i++) {
    if (strcmp(endpoints.ips[i], ip) == 0) {
      found = i;
      break;
    }
  }
  if (found == 0) return true;
  const uint8_t end = found < endpoints.count
                          ? found
                          : (endpoints.count < ENDPOINT_CANDIDATE_CAP ? endpoints.count++ : ENDPOINT_CANDIDATE_CAP - 1);
  for (uint8_t i = end; i > 0; i--) memcpy(endpoints.ips[i], endpoints.ips[i - 1], sizeof(endpoints.ips[i]));
  snprintf(endpoints.ips[0], sizeof(endpoints.ips[0]), "%s", ip);
  return true;
}

}  // namespace Net
}  // namespace AgentDeck
