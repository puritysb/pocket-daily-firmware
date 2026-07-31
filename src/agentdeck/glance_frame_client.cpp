// HttpDownloader.h must precede anything that pulls lwip (WiFi/ArduinoJson via
// Arduino.h) — its SdFat macros collide with lwip's ip4_addr.h otherwise. See
// the same note at the top of src/network/OtaUpdater.cpp.
#include "network/HttpDownloader.h"

#include <HalStorage.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "agent/AgentLog.h"
#include "glance_frame_client.h"

namespace AgentDeck {
namespace GlanceFrame {
namespace {

constexpr const char* kCachePath = "/.crosspoint/agentdeck-glance-frame.bin";
constexpr const char* kTmpPath = "/.crosspoint/agentdeck-glance-frame.tmp";

}  // namespace

const char* cachePath() { return kCachePath; }

Fetch fetchToCache(const char* ip, uint16_t port, const char* token, const char* board, size_t expectedBytes,
                   const char* echoSig, char* sigOut, size_t sigCap) {
  if (!ip || !ip[0] || !port || !board || expectedBytes == 0 || !Storage.ready()) return Fetch::Failed;

  char url[240];
  size_t o = (size_t)snprintf(url, sizeof(url), "http://%s:%u/glance-frame?board=%s", ip, (unsigned)port, board);
  if (token && token[0]) o += (size_t)snprintf(url + o, sizeof(url) - o, "&token=%s", token);
  if (echoSig && echoSig[0]) snprintf(url + o, sizeof(url) - o, "&sig=%s", echoSig);

  HttpDownloader::HeaderCapture sig;
  sig.name = "X-Frame-Sig";
  const auto r = HttpDownloader::downloadToFile(url, kTmpPath, nullptr, nullptr, "", "", &sig);

  if (r == HttpDownloader::NOT_MODIFIED) {
    // Bodyless by design; the tmp file was discarded by downloadToFile. The
    // echoSig can only have come from a Fresh fetch that validated the cache.
    return Storage.exists(kCachePath) ? Fetch::Unchanged : Fetch::Failed;
  }
  if (r != HttpDownloader::OK) return Fetch::Failed;

  // Only a complete frame may replace the cache: the blit is a straight
  // file→framebuffer copy, so a short or oversized body must never land.
  {
    HalFile f;
    if (!Storage.openFileForRead("AGENT", kTmpPath, f)) return Fetch::Failed;
    const size_t got = f.size();
    f.close();
    if (got != expectedBytes) {
      AgentLog::line("AGENT", "glance frame size mismatch: got %u want %u", (unsigned)got, (unsigned)expectedBytes);
      Storage.remove(kTmpPath);
      return Fetch::Failed;
    }
  }
  Storage.remove(kCachePath);
  if (!Storage.rename(kTmpPath, kCachePath)) {
    Storage.remove(kTmpPath);
    return Fetch::Failed;
  }
  if (sigOut && sigCap) snprintf(sigOut, sigCap, "%s", sig.value);
  return Fetch::Fresh;
}

}  // namespace GlanceFrame
}  // namespace AgentDeck
