// HttpDownloader.h must precede anything that pulls lwip (WiFi/ArduinoJson via
// Arduino.h) — its SdFat macros collide with lwip's ip4_addr.h otherwise. See
// the same note at the top of src/network/OtaUpdater.cpp.
#include "network/HttpDownloader.h"

#include <ArduinoJson.h>
#include <HalStorage.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include "agent/AgentLog.h"
#include "feed_client.h"
#include "outbox_store.h"
#include "protocol.h"

namespace AgentDeck {
namespace Feed {
namespace {

constexpr const char* kDir = "/.crosspoint";
constexpr const char* kEndpointPath = "/.crosspoint/agentdeck-endpoint.bin";
constexpr const char* kEndpointTmpPath = "/.crosspoint/agentdeck-endpoint.tmp";

struct EndpointRecord {
  uint32_t magic;
  char ip[16];
  uint16_t port;
  char token[40];
};
constexpr uint32_t kEndpointMagic = 0x31454441;  // "ADE1" (LE)

void buildUrl(char* out, size_t cap, const char* ip, uint16_t port, const char* path, const char* token) {
  if (token && token[0]) {
    snprintf(out, cap, "http://%s:%u%s?token=%s", ip, (unsigned)port, path, token);
  } else {
    snprintf(out, cap, "http://%s:%u%s", ip, (unsigned)port, path);
  }
}

// Drain the persisted outbox via POST /outbox. Every acknowledged decision is
// removed regardless of per-decision status (expired/rejected are terminal by
// contract). Returns false only on transport failure — records then stay
// queued for the next sync.
bool pushOutbox(const char* ip, uint16_t port, const char* token, const char* board) {
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
  if (!HttpDownloader::postJson(url, json.c_str(), json.size(), response)) {
    AgentLog::line("FEED", "outbox push failed (%u pending kept)", (unsigned)q.count);
    return false;
  }

  // Per-decision results are logged for the SD forensics trail; the queue is
  // cleared wholesale because acknowledgement is terminal either way.
  JsonDocument res;
  if (deserializeJson(res, response) == DeserializationError::Ok) {
    unsigned applied = 0, other = 0;
    for (JsonObject r : res["results"].as<JsonArray>()) {
      if (strcmp(r["status"] | "", "applied") == 0) applied++;
      else other++;
    }
    AgentLog::line("FEED", "outbox push: %u applied, %u expired/rejected", applied, other);
  }
  OutboxStore::Queue empty;
  memset(&empty, 0, sizeof(empty));
  OutboxStore::save(empty);
  return true;
}

}  // namespace

SyncResult syncOnce(const char* ip, uint16_t port, const char* token, const char* board) {
  SyncResult out;
  if (!ip || !ip[0] || !port) return out;

  // Outbox first: a queued decision is older than anything the feed will say,
  // and the feed we pull next should already reflect its effect.
  pushOutbox(ip, port, token, board);

  char url[160];
  buildUrl(url, sizeof(url), ip, port, "/feed", token);
  std::string bodyStr;
  if (!HttpDownloader::fetchUrl(url, bodyStr)) {
    AgentLog::line("FEED", "feed pull failed: %s:%u", ip, (unsigned)port);
    return out;
  }
  uint32_t nextPullSec = 0;
  if (!Protocol::applyCardFeed(bodyStr.c_str(), bodyStr.size(), &nextPullSec)) return out;

  out.ok = true;
  out.nextPullSec = nextPullSec;
  saveEndpoint(ip, port, token);
  return out;
}

bool loadEndpoint(char* ip, size_t ipCap, uint16_t& port, char* token, size_t tokenCap) {
  if (!Storage.ready() || !Storage.exists(kEndpointPath)) return false;
  HalFile f = Storage.open(kEndpointPath, O_RDONLY);
  if (!f) return false;
  EndpointRecord rec{};
  if (f.read(&rec, sizeof(rec)) != (int)sizeof(rec)) return false;
  if (rec.magic != kEndpointMagic || rec.ip[0] == '\0' || rec.port == 0) return false;
  rec.ip[sizeof(rec.ip) - 1] = '\0';
  rec.token[sizeof(rec.token) - 1] = '\0';
  snprintf(ip, ipCap, "%s", rec.ip);
  snprintf(token, tokenCap, "%s", rec.token);
  port = rec.port;
  return true;
}

bool saveEndpoint(const char* ip, uint16_t port, const char* token) {
  if (!Storage.ready() || !ip || !ip[0] || !port) return false;
  // Skip the SD write when nothing changed — this runs on every WS connect
  // and every successful pull.
  {
    char curIp[16] = {0};
    char curToken[40] = {0};
    uint16_t curPort = 0;
    if (loadEndpoint(curIp, sizeof(curIp), curPort, curToken, sizeof(curToken)) && curPort == port &&
        strcmp(curIp, ip) == 0 && strcmp(curToken, token ? token : "") == 0) {
      return true;
    }
  }
  Storage.mkdir(kDir);
  EndpointRecord rec{};
  rec.magic = kEndpointMagic;
  snprintf(rec.ip, sizeof(rec.ip), "%s", ip);
  rec.port = port;
  snprintf(rec.token, sizeof(rec.token), "%s", token ? token : "");
  {
    HalFile f = Storage.open(kEndpointTmpPath, O_WRITE | O_CREAT | O_TRUNC);
    if (!f) return false;
    if (f.write(&rec, sizeof(rec)) != sizeof(rec)) return false;
  }
  Storage.remove(kEndpointPath);
  if (!Storage.rename(kEndpointTmpPath, kEndpointPath)) return false;
  AgentLog::line("FEED", "endpoint cached: %s:%u", ip, (unsigned)port);
  return true;
}

}  // namespace Feed
}  // namespace AgentDeck
