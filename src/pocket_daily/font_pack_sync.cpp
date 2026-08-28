// HttpDownloader must precede Arduino/Wi-Fi headers pulled by its dependencies.
#include "pocket_daily/font_pack_sync.h"

#include <HalStorage.h>
#include <MD5Builder.h>

#include <cctype>
#include <cstdio>
#include <cstring>

#include "agent/AgentLog.h"
#include "network/HttpDownloader.h"
#include "pocket_daily/surface_request.h"

namespace PocketDaily {
namespace FontPackSync {
namespace {

constexpr uint32_t STATE_MAGIC = 0x31534650;  // "PFS1" little-endian

struct State {
  uint32_t magic = STATE_MAGIC;
  uint32_t contentVersion = 0;
  uint32_t size = 0;
  char md5[33] = {0};
};

bool validMd5(const char* value) {
  if (!value || strlen(value) != 32) return false;
  for (size_t i = 0; i < 32; ++i) {
    if (!isxdigit(static_cast<unsigned char>(value[i]))) return false;
  }
  return true;
}

bool ensureDirectories() {
  if (!Storage.exists("/.fonts") && !Storage.mkdir("/.fonts")) return false;
  if (!Storage.exists(FONT_DIR) && !Storage.mkdir(FONT_DIR)) return false;
  if (!Storage.exists("/pocket-daily") && !Storage.mkdir("/pocket-daily")) return false;
  return Storage.exists(STATE_DIR) || Storage.mkdir(STATE_DIR);
}

bool fileSize(const char* path, uint32_t& out) {
  HalFile file = Storage.open(path, O_RDONLY);
  if (!file) return false;
  out = static_cast<uint32_t>(file.size());
  return true;
}

bool fileMd5(const char* path, char out[33]) {
  out[0] = '\0';
  HalFile file = Storage.open(path, O_RDONLY);
  if (!file) return false;
  MD5Builder md5;
  md5.begin();
  uint8_t buffer[256];
  while (true) {
    const int got = file.read(buffer, sizeof(buffer));
    if (got < 0) return false;
    if (got == 0) break;
    md5.add(buffer, static_cast<size_t>(got));
  }
  md5.calculate();
  md5.getChars(out);
  out[32] = '\0';
  return true;
}

bool validHeader(const char* path) {
  uint8_t header[32] = {0};
  HalFile file = Storage.open(path, O_RDONLY);
  if (!file || file.read(header, sizeof(header)) != sizeof(header)) return false;
  static constexpr uint8_t magic[8] = {'C', 'P', 'F', 'O', 'N', 'T', 0, 0};
  const uint16_t version = static_cast<uint16_t>(header[8] | (header[9] << 8));
  const uint8_t styleCount = header[12];
  return memcmp(header, magic, sizeof(magic)) == 0 && version == FORMAT_VERSION && styleCount > 0 && styleCount <= 4;
}

bool loadState(State& state) {
  HalFile file = Storage.open(STATE_PATH, O_RDONLY);
  return file && file.read(&state, sizeof(state)) == sizeof(state) && state.magic == STATE_MAGIC;
}

bool saveState(const Advert& advert) {
  State state;
  state.contentVersion = advert.contentVersion;
  state.size = advert.size;
  snprintf(state.md5, sizeof(state.md5), "%s", advert.md5);
  HalFile file = Storage.open(STATE_PATH, O_WRITE | O_CREAT | O_TRUNC);
  return file && file.write(&state, sizeof(state)) == sizeof(state);
}

bool isCurrent(const Advert& advert) {
  State state;
  uint32_t installedSize = 0;
  return loadState(state) && Storage.exists(FONT_PATH) && fileSize(FONT_PATH, installedSize) &&
         installedSize == advert.size && state.contentVersion >= advert.contentVersion && state.size == advert.size &&
         strcasecmp(state.md5, advert.md5) == 0;
}

bool installCandidate(const Advert& advert) {
  const bool hadActive = Storage.exists(FONT_PATH);
  Storage.remove(BACKUP_PATH);
  if (hadActive && !Storage.rename(FONT_PATH, BACKUP_PATH)) return false;
  if (!Storage.rename(TEMP_PATH, FONT_PATH)) {
    if (hadActive) Storage.rename(BACKUP_PATH, FONT_PATH);
    return false;
  }
  if (!saveState(advert)) {
    Storage.remove(FONT_PATH);
    if (hadActive) Storage.rename(BACKUP_PATH, FONT_PATH);
    return false;
  }
  Storage.remove(BACKUP_PATH);
  return true;
}

}  // namespace

Result sync(const char* ip, uint16_t port, const char* token, const char* board, const Advert& advert) {
  if (!advert.packageId[0]) return Result::NotAdvertised;
  if (!ip || !ip[0] || !port || strcmp(advert.packageId, PACKAGE_ID) != 0 || advert.contentVersion == 0 ||
      advert.formatVersion != FORMAT_VERSION || advert.size < 64 || advert.size > MAX_PACK_BYTES ||
      !validMd5(advert.md5) || strcmp(advert.licenseSpdx, "OFL-1.1") != 0) {
    AgentLog::line("FONT", "pack advert rejected: id=%s version=%lu format=%u size=%lu", advert.packageId,
                   (unsigned long)advert.contentVersion, (unsigned)advert.formatVersion, (unsigned long)advert.size);
    return Result::Failed;
  }
  if (isCurrent(advert)) return Result::Current;
  if (!ensureDirectories()) return Result::Failed;

  char url[320];
  size_t offset = static_cast<size_t>(snprintf(url, sizeof(url), "http://%s:%u/fonts/pack?id=%s&version=%lu", ip,
                                               (unsigned)port, PACKAGE_ID, (unsigned long)advert.contentVersion));
  if (token && token[0] && offset < sizeof(url)) snprintf(url + offset, sizeof(url) - offset, "&token=%s", token);

  const SurfaceRequestHeaders identity(board);
  const auto requestHeaders = identity.view();
  const auto downloaded =
      HttpDownloader::downloadToFile(url, TEMP_PATH, nullptr, nullptr, "", "", nullptr, &requestHeaders);
  if (downloaded != HttpDownloader::OK) {
    AgentLog::line("FONT", "pack download failed: code=%d", (int)downloaded);
    return Result::Failed;
  }

  uint32_t actualSize = 0;
  char actualMd5[33] = {0};
  const bool accepted = fileSize(TEMP_PATH, actualSize) && actualSize == advert.size && fileMd5(TEMP_PATH, actualMd5) &&
                        strcasecmp(actualMd5, advert.md5) == 0 && validHeader(TEMP_PATH);
  if (!accepted) {
    AgentLog::line("FONT", "pack verification failed: size=%lu/%lu md5=%s/%s", (unsigned long)actualSize,
                   (unsigned long)advert.size, actualMd5, advert.md5);
    Storage.remove(TEMP_PATH);
    return Result::Failed;
  }
  if (!installCandidate(advert)) {
    Storage.remove(TEMP_PATH);
    return Result::Failed;
  }
  AgentLog::line("FONT", "installed %s v%lu (%lu bytes)", FAMILY_NAME, (unsigned long)advert.contentVersion,
                 (unsigned long)advert.size);
  return Result::Updated;
}

}  // namespace FontPackSync
}  // namespace PocketDaily
