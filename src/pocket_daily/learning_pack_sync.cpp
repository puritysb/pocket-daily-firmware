// HttpDownloader must be included before Wi-Fi/lwIP dependencies pulled by
// Arduino headers; see the same ordering constraint in feed_client.cpp.
#include "pocket_daily/learning_pack_sync.h"

#include <HalStorage.h>
#include <MD5Builder.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

#include "agent/AgentLog.h"
#include "network/HttpDownloader.h"
#include "pocket_daily/learning_pack.h"
#include "pocket_daily/surface_request.h"

namespace PocketDaily {
namespace LearningPackSync {
namespace {

bool validMd5(const char* value) {
  if (!value || strlen(value) != 32) return false;
  for (size_t i = 0; i < 32; ++i) {
    if (!isxdigit(static_cast<unsigned char>(value[i]))) return false;
  }
  return true;
}

bool ensureDirectories() {
  if (!Storage.exists("/pocket-daily") && !Storage.mkdir("/pocket-daily")) return false;
  return Storage.exists(LearningPack::PACK_DIR) || Storage.mkdir(LearningPack::PACK_DIR);
}

bool fileMd5(const char* path, char out[33]) {
  out[0] = '\0';
  HalFile file = Storage.open(path, O_RDONLY);
  if (!file) return false;
  MD5Builder md5;
  md5.begin();
  uint8_t buffer[1024];
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

}  // namespace

Result sync(const char* ip, uint16_t port, const char* token, const char* board, const Advert& advert) {
  if (!advert.packageId[0]) return Result::NotAdvertised;
  if (!ip || !ip[0] || !port || strcmp(advert.packageId, LearningPack::PACKAGE_ID) != 0 ||
      advert.contentVersion == 0 || advert.formatVersion != LearningPack::FORMAT_VERSION ||
      advert.size < sizeof(LearningPack::Header) || advert.size > LearningPack::MAX_PACK_BYTES ||
      !validMd5(advert.md5) || !advert.licenseSpdx[0]) {
    AgentLog::line("LEARN", "pack advert rejected: id=%s version=%lu format=%u size=%lu", advert.packageId,
                   (unsigned long)advert.contentVersion, (unsigned)advert.formatVersion, (unsigned long)advert.size);
    return Result::Failed;
  }

  LearningPack::Metadata installed{};
  if (LearningPack::ensureAvailable(&installed) && installed.contentVersion >= advert.contentVersion) {
    return Result::Current;
  }
  if (!ensureDirectories()) return Result::Failed;

  char url[320];
  size_t offset = static_cast<size_t>(snprintf(url, sizeof(url),
                                               "http://%s:%u/learning/pack?id=%s&version=%lu", ip,
                                               (unsigned)port, LearningPack::PACKAGE_ID,
                                               (unsigned long)advert.contentVersion));
  if (token && token[0] && offset < sizeof(url))
    snprintf(url + offset, sizeof(url) - offset, "&token=%s", token);

  const SurfaceRequestHeaders identity(board);
  const auto requestHeaders = identity.view();
  const auto downloaded = HttpDownloader::downloadToFile(url, LearningPack::PACK_TEMP_PATH, nullptr, nullptr, "", "",
                                                          nullptr, &requestHeaders);
  if (downloaded != HttpDownloader::OK) {
    AgentLog::line("LEARN", "pack download failed: code=%d", (int)downloaded);
    return Result::Failed;
  }

  HalFile candidate = Storage.open(LearningPack::PACK_TEMP_PATH, O_RDONLY);
  const uint32_t actualSize = candidate ? static_cast<uint32_t>(candidate.size()) : 0;
  candidate.close();
  char actualMd5[33] = {0};
  LearningPack::Metadata metadata{};
  const bool accepted = actualSize == advert.size && fileMd5(LearningPack::PACK_TEMP_PATH, actualMd5) &&
                        strcasecmp(actualMd5, advert.md5) == 0 &&
                        LearningPack::validate(LearningPack::PACK_TEMP_PATH, &metadata) &&
                        metadata.contentVersion == advert.contentVersion &&
                        strcmp(metadata.packageId, advert.packageId) == 0 &&
                        strcmp(metadata.licenseSpdx, advert.licenseSpdx) == 0;
  if (!accepted) {
    AgentLog::line("LEARN", "pack verification failed: size=%lu/%lu md5=%s/%s",
                   (unsigned long)actualSize, (unsigned long)advert.size, actualMd5, advert.md5);
    Storage.remove(LearningPack::PACK_TEMP_PATH);
    return Result::Failed;
  }
  if (!LearningPack::install(LearningPack::PACK_TEMP_PATH, &installed)) {
    Storage.remove(LearningPack::PACK_TEMP_PATH);
    return Result::Failed;
  }
  return Result::Updated;
}

}  // namespace LearningPackSync
}  // namespace PocketDaily
