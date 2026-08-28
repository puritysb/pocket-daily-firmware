// HttpDownloader.h must precede anything that pulls lwip (WiFi/ArduinoJson via
// Arduino.h) — its SdFat macros collide with lwip's ip4_addr.h otherwise. See
// the same note at the top of src/network/OtaUpdater.cpp.
#include "feed_client.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <WiFi.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include "agent/AgentLog.h"
#include "auth_store.h"
#include "network/HttpDownloader.h"
#include "outbox_store.h"
#include "pocket_daily/surface_request.h"
#include "protocol.h"

namespace AgentDeck {
namespace Feed {
namespace {

constexpr const char* kDir = "/.crosspoint";
constexpr const char* kEndpointPath = "/.crosspoint/agentdeck-endpoint.bin";
constexpr const char* kEndpointTmpPath = "/.crosspoint/agentdeck-endpoint.tmp";

struct EndpointRecordV1 {
  uint32_t magic;
  char ip[16];
  uint16_t port;
  char token[40];
};
constexpr uint32_t kEndpointMagicV1 = 0x31454441;  // "ADE1" (LE)

struct EndpointRecordV2 {
  uint32_t magic;
  char ips[Net::ENDPOINT_CANDIDATE_CAP][16];
  uint8_t count;
  uint8_t reserved;
  uint16_t port;
  char token[40];
};
constexpr uint32_t kEndpointMagicV2 = 0x32454441;  // "ADE2" (LE)

void buildUrl(char* out, size_t cap, const char* ip, uint16_t port, const char* path, const char* token) {
  if (token && token[0]) {
    snprintf(out, cap, "http://%s:%u%s?token=%s", ip, (unsigned)port, path, token);
  } else {
    snprintf(out, cap, "http://%s:%u%s", ip, (unsigned)port, path);
  }
}

bool redirectedIpv4(const HttpDownloader::EffectiveUrlCapture& effectiveUrl, char* out, size_t cap) {
  if (!out || cap == 0 || strncmp(effectiveUrl.value, "http://", 7) != 0) return false;
  char ip[16] = {0};
  if (sscanf(effectiveUrl.value + 7, "%15[0-9.]", ip) != 1 || !ip[0]) return false;
  snprintf(out, cap, "%s", ip);
  return true;
}

// Drain the persisted outbox via POST /outbox. Every acknowledged decision is
// removed regardless of per-decision status (expired/rejected are terminal by
// contract). Returns false only on transport failure — records then stay
// queued for the next sync.
bool pushOutbox(const char* ip, uint16_t port, const char* token, const char* board, char* effectiveIp,
                size_t effectiveIpCap) {
  if (effectiveIp && effectiveIpCap) snprintf(effectiveIp, effectiveIpCap, "%s", ip ? ip : "");
  OutboxStore::Queue q;
  if (!OutboxStore::load(q) || q.count == 0) return true;

  JsonDocument body;
  body["board"] = board;
  JsonArray decisions = body["decisions"].to<JsonArray>();
  for (uint8_t i = 0; i < q.count; i++) {
    const OutboxStore::Record& r = q.records[i];
    JsonObject d = decisions.add<JsonObject>();
    d["cardId"] = r.cardId;
    if (r.sessionId[0]) d["sessionId"] = r.sessionId;
    if (r.requestId[0]) d["requestId"] = r.requestId;
    d["action"] = r.action;
    if (r.choiceId[0]) d["choiceId"] = r.choiceId;
    if (r.decision[0]) d["decision"] = r.decision;
    if (r.index >= 0) d["index"] = r.index;
    if (r.value[0]) d["value"] = r.value;
    if (r.question[0]) d["question"] = r.question;
    // Drift-free age: device epoch clocks aren't trusted, so send elapsed
    // seconds when a clock source existed at record time.
    const uint32_t now = (uint32_t)time(nullptr);
    if (r.recordedEpoch && now > 1700000000u && now > r.recordedEpoch) d["ageSec"] = now - r.recordedEpoch;
  }
  std::string json;
  serializeJson(body, json);

  char url[160];
  buildUrl(url, sizeof(url), ip, port, "/outbox", token);
  std::string response;
  const PocketDaily::SurfaceRequestHeaders surfaceIdentity(board);
  const auto requestHeaders = surfaceIdentity.view();
  HttpDownloader::EffectiveUrlCapture effectiveUrl;
  if (!HttpDownloader::postJson(url, json.c_str(), json.size(), response, 16384, &requestHeaders, &effectiveUrl)) {
    AgentLog::line("FEED", "outbox push failed (%u pending kept)", (unsigned)q.count);
    return false;
  }
  redirectedIpv4(effectiveUrl, effectiveIp, effectiveIpCap);

  // Per-decision results are logged for the SD forensics trail; the queue is
  // cleared wholesale because acknowledgement is terminal either way.
  JsonDocument res;
  if (deserializeJson(res, response) == DeserializationError::Ok) {
    unsigned applied = 0, other = 0;
    for (JsonObject r : res["results"].as<JsonArray>()) {
      if (strcmp(r["status"] | "", "applied") == 0)
        applied++;
      else
        other++;
    }
    AgentLog::line("FEED", "outbox push: %u applied, %u expired/rejected", applied, other);
  }
  OutboxStore::Queue empty;
  memset(&empty, 0, sizeof(empty));
  OutboxStore::save(empty);
  return true;
}

}  // namespace

SyncResult syncOnce(const char* ip, uint16_t port, const char* token, const char* board, const char* echoSig,
                    const SyncTelemetry& telemetry) {
  SyncResult out;
  if (!ip || !ip[0] || !port) return out;

  char routeIp[16] = {0};
  snprintf(routeIp, sizeof(routeIp), "%s", ip);

  // Outbox first: a queued decision is older than anything the feed will say,
  // and the feed we pull next should already reflect its effect.
  pushOutbox(ip, port, token, board, routeIp, sizeof(routeIp));

  char url[224];
  buildUrl(url, sizeof(url), routeIp, port, "/feed", token);
  // Conditional pull + telemetry ride the query string (the GET is bodyless).
  // buildUrl already appended ?token=… when a token exists.
  size_t o = strlen(url);
  const char sep0 = strchr(url, '?') ? '&' : '?';
  bool first = true;
  auto app = [&](const char* fmt, auto value) {
    o += snprintf(url + o, sizeof(url) - o, "%c", first ? sep0 : '&');
    o += snprintf(url + o, sizeof(url) - o, fmt, value);
    first = false;
  };
  if (echoSig && echoSig[0]) app("sig=%s", echoSig);
  // New Pocket-reader firmware asks for daemon-authored portable cards only.
  // Older firmware omits this parameter and keeps receiving session rows.
  app("surface=%s", PocketDaily::LEGACY_SURFACE_QUERY);
  // Board identity lets the daemon attach a board-targeted `fw` staging advert
  // (contract § Pull OTA) without relying on its IP→board memory.
  if (board && board[0]) app("board=%s", board);
  if (telemetry.battPct >= 0) app("batt=%d", telemetry.battPct);
  if (telemetry.rssiDbm < 0) app("rssi=%d", telemetry.rssiDbm);

  std::string bodyStr;
  const PocketDaily::SurfaceRequestHeaders surfaceIdentity(board);
  const auto requestHeaders = surfaceIdentity.view();
  HttpDownloader::EffectiveUrlCapture effectiveUrl;
  if (!HttpDownloader::fetchUrl(url, bodyStr, "", "", &requestHeaders, &effectiveUrl)) {
    AgentLog::line("FEED", "feed pull failed: %s:%u", routeIp, (unsigned)port);
    return out;
  }
  redirectedIpv4(effectiveUrl, routeIp, sizeof(routeIp));
  const Protocol::FeedApply applied = Protocol::applyCardFeed(bodyStr.c_str(), bodyStr.size());
  if (!applied.ok) return out;

  out.ok = true;
  snprintf(out.endpointIp, sizeof(out.endpointIp), "%s", routeIp);
  out.unchanged = applied.unchanged;
  out.nextPullSec = applied.nextPullSec;
  strncpy(out.deckSig, applied.deckSig, sizeof(out.deckSig) - 1);
  out.learningPack = applied.learningPack;
  if (applied.fwSize) {
    if (PocketDaily::otaIdentityMatches(applied.fwProductId, applied.fwBoard, applied.fwUpdateChannel, board)) {
      out.fwSize = applied.fwSize;
      strncpy(out.fwMd5, applied.fwMd5, sizeof(out.fwMd5) - 1);
    } else {
      AgentLog::line("OTA", "feed firmware rejected: tuple product=%s board=%s channel=%s", applied.fwProductId,
                     applied.fwBoard, applied.fwUpdateChannel);
    }
  }
  saveEndpoint(routeIp, port, token);
  return out;
}

bool loadEndpointCandidates(Net::EndpointCandidates& endpoints, char* token, size_t tokenCap) {
  endpoints = {};
  if (token && tokenCap) token[0] = '\0';
  if (!Storage.ready() || !Storage.exists(kEndpointPath)) return false;
  HalFile f = Storage.open(kEndpointPath, O_RDONLY);
  if (!f) return false;
  uint32_t magic = 0;
  if (f.read(&magic, sizeof(magic)) != (int)sizeof(magic) || !f.seekSet(0)) return false;
  char storedToken[40] = {0};
  if (magic == kEndpointMagicV2) {
    EndpointRecordV2 rec{};
    if (f.read(&rec, sizeof(rec)) != (int)sizeof(rec) || rec.port == 0) return false;
    rec.count = rec.count > Net::ENDPOINT_CANDIDATE_CAP ? Net::ENDPOINT_CANDIDATE_CAP : rec.count;
    endpoints.port = rec.port;
    for (uint8_t i = 0; i < rec.count; i++) {
      rec.ips[i][sizeof(rec.ips[i]) - 1] = '\0';
      Net::endpointCandidateAdd(endpoints, rec.ips[i]);
    }
    rec.token[sizeof(rec.token) - 1] = '\0';
    snprintf(storedToken, sizeof(storedToken), "%s", rec.token);
  } else if (magic == kEndpointMagicV1) {
    EndpointRecordV1 rec{};
    if (f.read(&rec, sizeof(rec)) != (int)sizeof(rec) || rec.ip[0] == '\0' || rec.port == 0) return false;
    rec.ip[sizeof(rec.ip) - 1] = '\0';
    rec.token[sizeof(rec.token) - 1] = '\0';
    endpoints.port = rec.port;
    Net::endpointCandidateAdd(endpoints, rec.ip);
    snprintf(storedToken, sizeof(storedToken), "%s", rec.token);
  } else {
    return false;
  }
  if (!endpoints.count) return false;
  // NVS is authoritative. An older endpoint record may contain a stale token
  // from before credentials moved out of the user-browsable SD filesystem.
  char nvsToken[40] = {0};
  if (AuthStore::load(nvsToken, sizeof(nvsToken))) snprintf(storedToken, sizeof(storedToken), "%s", nvsToken);
  if (token && tokenCap) snprintf(token, tokenCap, "%s", storedToken);
  return true;
}

bool loadEndpoint(char* ip, size_t ipCap, uint16_t& port, char* token, size_t tokenCap) {
  Net::EndpointCandidates endpoints;
  if (!loadEndpointCandidates(endpoints, token, tokenCap)) return false;
  snprintf(ip, ipCap, "%s", endpoints.ips[0]);
  port = endpoints.port;
  return true;
}

bool saveEndpointCandidates(const Net::EndpointCandidates& endpoints, const char* token, bool mergeWithCached) {
  if (!Storage.ready() || !endpoints.count || !endpoints.port) return false;

  // Candidate ORDER is learned knowledge, not a discovery detail: saveEndpoint()
  // promotes whichever address actually answered to index 0. Discovery must not
  // throw that away. It used to: a dual-homed host advertises both its wired and
  // wireless address, one of which may be unreachable from this device, and a UDP
  // beacon carries only ONE address. Replacing the cache with the beacon's single
  // entry deleted the address that had just been proven to work, so every
  // rediscovery walked the device back onto the dead route (observed on this
  // network: preferred=...68.60 -> preferred=...68.100, then repeated failures).
  //
  // Merge instead. Cached order comes first, discovery only contributes addresses
  // we have not seen. A head that has genuinely gone away costs one bounded
  // timeout before the loop reaches a live candidate — and that success promotes
  // it back to the front, so the ordering is self-healing either way.
  Net::EndpointCandidates normalized;
  normalized.port = endpoints.port;
  if (mergeWithCached) {
    Net::EndpointCandidates cached;
    char cachedToken[40] = {0};
    if (loadEndpointCandidates(cached, cachedToken, sizeof(cachedToken)) && cached.port == endpoints.port) {
      for (uint8_t i = 0; i < cached.count; i++) Net::endpointCandidateAdd(normalized, cached.ips[i]);
    }
  }
  for (uint8_t i = 0; i < endpoints.count && i < Net::ENDPOINT_CANDIDATE_CAP; i++) {
    Net::endpointCandidateAdd(normalized, endpoints.ips[i]);
  }
  if (!normalized.count) return false;

  // Skip the SD write when nothing changed — this runs on every successful
  // pull and the cache is also mirrored on the user-visible SD card.
  Net::EndpointCandidates current;
  char currentToken[40] = {0};
  if (loadEndpointCandidates(current, currentToken, sizeof(currentToken)) && current.port == normalized.port &&
      current.count == normalized.count && strcmp(currentToken, token ? token : "") == 0 &&
      memcmp(current.ips, normalized.ips, sizeof(current.ips)) == 0) {
    return true;
  }
  Storage.mkdir(kDir);
  EndpointRecordV2 rec{};
  rec.magic = kEndpointMagicV2;
  memcpy(rec.ips, normalized.ips, sizeof(rec.ips));
  rec.count = normalized.count;
  rec.port = normalized.port;
  snprintf(rec.token, sizeof(rec.token), "%s", token ? token : "");
  {
    HalFile f = Storage.open(kEndpointTmpPath, O_WRITE | O_CREAT | O_TRUNC);
    if (!f) return false;
    if (f.write(&rec, sizeof(rec)) != sizeof(rec)) return false;
  }
  Storage.remove(kEndpointPath);
  if (!Storage.rename(kEndpointTmpPath, kEndpointPath)) return false;
  AgentLog::line("FEED", "endpoints cached: preferred=%s:%u candidates=%u", normalized.ips[0],
                 (unsigned)normalized.port, (unsigned)normalized.count);
  return true;
}

bool orderByReachability(Net::EndpointCandidates& endpoints, uint32_t probeTimeoutMs) {
  if (endpoints.count < 2 || !endpoints.port) return false;
  for (uint8_t i = 0; i < endpoints.count; i++) {
    // WiFiClient is stack-light and its socket is released at scope exit; the
    // probe runs before any HTTP client exists, so it never competes with the
    // request's buffers for contiguous heap.
    WiFiClient probe;
    const bool up = probe.connect(endpoints.ips[i], endpoints.port, (int32_t)probeTimeoutMs);
    probe.stop();
    if (!up) continue;
    if (i == 0) return false;  // already in the right order
    AgentLog::line("FEED", "endpoint probe: %s answered, %s did not", endpoints.ips[i], endpoints.ips[0]);
    Net::endpointCandidatePromote(endpoints, endpoints.ips[i]);
    return true;
  }
  // Nothing answered — the radio or the daemon is down, not the ordering.
  // Leave the learned order untouched.
  return false;
}

bool saveEndpoint(const char* ip, uint16_t port, const char* token) {
  if (!Storage.ready() || !ip || !ip[0] || !port) return false;
  Net::EndpointCandidates endpoints;
  char ignoredToken[40] = {0};
  // Preserve alternatives only when they belong to the same service port.
  // A daemon moving to a fallback port is a new endpoint set.
  if (!loadEndpointCandidates(endpoints, ignoredToken, sizeof(ignoredToken)) || endpoints.port != port) {
    endpoints = {};
    endpoints.port = port;
  }
  Net::endpointCandidatePromote(endpoints, ip);
  // Already merged from cache above; merging again would re-front the old order.
  return saveEndpointCandidates(endpoints, token, /*mergeWithCached=*/false);
}

}  // namespace Feed
}  // namespace AgentDeck
