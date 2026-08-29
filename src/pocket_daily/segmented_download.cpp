// HttpDownloader must precede Arduino/Wi-Fi headers pulled by its dependencies.
#include "pocket_daily/segmented_download.h"

#include <HalStorage.h>
#include <MD5Builder.h>
#include <esp_wifi.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <strings.h>

#include "agent/AgentLog.h"
#include "network/HttpDownloader.h"
#include "pocket_daily/surface_request.h"

namespace PocketDaily {
namespace SegmentedDownload {
namespace {

// 64 KiB measured well below one second on the local X3 link while avoiding
// the reconnect overhead of the 1 KiB WS path. A stalled request still inherits
// the LAN client's bounded timeout; controls run again after every response.
constexpr uint32_t kChunkBytes = 64U * 1024U;

struct Transfer {
  bool active = false;
  bool wifiPowerSaveSuspended = false;
  char ip[16] = {0};
  uint16_t port = 0;
  char token[40] = {0};
  char board[20] = {0};
  char requestPath[192] = {0};
  char tempPath[112] = {0};
  uint32_t expectedSize = 0;
  uint32_t written = 0;
  char expectedMd5[33] = {0};
};

Transfer transfer;
MD5Builder md5;

bool validMd5(const char* value) {
  if (!value || strlen(value) != 32) return false;
  for (size_t i = 0; i < 32; ++i) {
    if (!isxdigit(static_cast<unsigned char>(value[i]))) return false;
  }
  return true;
}

void restoreWifiPowerSave() {
  if (!transfer.wifiPowerSaveSuspended) return;
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  transfer.wifiPowerSaveSuspended = false;
}

void finish(bool removeCandidate) {
  char path[sizeof(transfer.tempPath)];
  snprintf(path, sizeof(path), "%s", transfer.tempPath);
  restoreWifiPowerSave();
  transfer = {};
  if (removeCandidate && path[0]) Storage.remove(path);
}

void adoptRedirectHost(const HttpDownloader::EffectiveUrlCapture& effectiveUrl) {
  if (strncmp(effectiveUrl.value, "http://", 7) != 0) return;
  char redirectedIp[16] = {0};
  if (sscanf(effectiveUrl.value + 7, "%15[0-9.]", redirectedIp) != 1 || !redirectedIp[0]) return;
  if (strcmp(transfer.ip, redirectedIp) == 0) return;
  AgentLog::line("ASSET", "using redirected download path %s", redirectedIp);
  snprintf(transfer.ip, sizeof(transfer.ip), "%s", redirectedIp);
}

}  // namespace

bool begin(const char* ip, uint16_t port, const char* token, const char* board, const char* requestPath,
           const char* tempPath, uint32_t expectedSize, const char* expectedMd5) {
  if (!ip || !ip[0] || !port || !board || !board[0] || !requestPath || requestPath[0] != '/' ||
      !strchr(requestPath, '?') || !tempPath || !tempPath[0] || !expectedSize || !validMd5(expectedMd5)) {
    return false;
  }
  cancel();
  transfer = {};
  if (snprintf(transfer.ip, sizeof(transfer.ip), "%s", ip) >= (int)sizeof(transfer.ip) ||
      snprintf(transfer.token, sizeof(transfer.token), "%s", token ? token : "") >= (int)sizeof(transfer.token) ||
      snprintf(transfer.board, sizeof(transfer.board), "%s", board) >= (int)sizeof(transfer.board) ||
      snprintf(transfer.requestPath, sizeof(transfer.requestPath), "%s", requestPath) >=
          (int)sizeof(transfer.requestPath) ||
      snprintf(transfer.tempPath, sizeof(transfer.tempPath), "%s", tempPath) >= (int)sizeof(transfer.tempPath)) {
    transfer = {};
    return false;
  }
  snprintf(transfer.expectedMd5, sizeof(transfer.expectedMd5), "%s", expectedMd5);
  transfer.port = port;
  transfer.expectedSize = expectedSize;
  Storage.remove(transfer.tempPath);
  HalFile candidate = Storage.open(transfer.tempPath, O_RDWR | O_CREAT | O_TRUNC);
  if (!candidate) {
    transfer = {};
    return false;
  }
  candidate.close();
  md5.begin();
  esp_wifi_set_ps(WIFI_PS_NONE);
  transfer.wifiPowerSaveSuspended = true;
  transfer.active = true;
  AgentLog::line("ASSET", "cooperative transfer ready: %s (%u bytes)", transfer.requestPath,
                 (unsigned)transfer.expectedSize);
  return true;
}

Step service() {
  if (!transfer.active) return Step::Idle;
  const uint32_t before = transfer.written;
  const uint32_t target = std::min<uint32_t>(transfer.expectedSize, before + kChunkBytes);

  char url[384];
  size_t offset = (size_t)snprintf(url, sizeof(url), "http://%s:%u%s", transfer.ip, (unsigned)transfer.port,
                                   transfer.requestPath);
  if (transfer.token[0] && offset < sizeof(url))
    offset += (size_t)snprintf(url + offset, sizeof(url) - offset, "&token=%s", transfer.token);
  if (offset >= sizeof(url) ||
      snprintf(url + offset, sizeof(url) - offset, "&from=%lu&limit=%lu", (unsigned long)before,
               (unsigned long)kChunkBytes) >= (int)(sizeof(url) - offset)) {
    finish(true);
    return Step::Failed;
  }

  HalFile out = Storage.open(transfer.tempPath, O_RDWR | O_CREAT);
  if (!out || !out.seekSet(before)) {
    if (out) out.close();
    finish(true);
    return Step::Failed;
  }

  uint32_t written = before;
  bool writeFailed = false;
  bool responseOverflow = false;
  const SurfaceRequestHeaders surfaceIdentity(transfer.board);
  const auto requestHeaders = surfaceIdentity.view();
  HttpDownloader::EffectiveUrlCapture effectiveUrl;
  const bool requestComplete = HttpDownloader::fetchUrl(
      url,
      [&out, &written, target, &writeFailed, &responseOverflow](const uint8_t* data, size_t len) {
        const uint32_t room = target > written ? target - written : 0;
        const size_t accepted = std::min<size_t>(len, room);
        if (accepted && out.write(data, accepted) != accepted) {
          writeFailed = true;
          return false;
        }
        if (accepted) md5.add(data, accepted);
        written += (uint32_t)accepted;
        if (accepted != len) responseOverflow = true;
        return accepted == len;
      },
      "", "", &requestHeaders, &effectiveUrl);
  adoptRedirectHost(effectiveUrl);
  out.close();
  transfer.written = written;

  if (writeFailed || responseOverflow || written > transfer.expectedSize) {
    AgentLog::line("ASSET", "segment rejected at %u/%u", (unsigned)written, (unsigned)transfer.expectedSize);
    finish(true);
    return Step::Failed;
  }
  if (written >= transfer.expectedSize) {
    uint32_t actualSize = 0;
    HalFile complete = Storage.open(transfer.tempPath, O_RDONLY);
    if (complete) actualSize = (uint32_t)complete.size();
    complete.close();
    md5.calculate();
    char actualMd5[33] = {0};
    md5.getChars(actualMd5);
    if (actualSize != transfer.expectedSize || strcasecmp(actualMd5, transfer.expectedMd5) != 0) {
      AgentLog::line("ASSET", "verification failed: size=%u/%u md5=%s/%s", (unsigned)actualSize,
                     (unsigned)transfer.expectedSize, actualMd5, transfer.expectedMd5);
      finish(true);
      return Step::Failed;
    }
    restoreWifiPowerSave();
    transfer.active = false;
    AgentLog::line("ASSET", "download verified: %u bytes", (unsigned)actualSize);
    return Step::Complete;
  }
  if (written > before) {
    AgentLog::line("ASSET", "segment %u/%u", (unsigned)written, (unsigned)transfer.expectedSize);
    return Step::Progress;
  }
  (void)requestComplete;
  return Step::Retry;
}

void cancel() {
  if (!transfer.active && !transfer.tempPath[0]) return;
  finish(true);
}

bool active() { return transfer.active; }
uint32_t downloadedBytes() { return transfer.written; }
uint32_t totalBytes() { return transfer.expectedSize; }

}  // namespace SegmentedDownload
}  // namespace PocketDaily
