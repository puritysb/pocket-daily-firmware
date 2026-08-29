// HttpDownloader must be included before Wi-Fi/lwIP dependencies pulled by
// Arduino headers; see the same ordering constraint in feed_client.cpp.
#include "pocket_daily/learning_pack_sync.h"

#include <HalStorage.h>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "agent/AgentLog.h"
#include "pocket_daily/learning_pack.h"
#include "pocket_daily/segmented_download.h"

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

Advert activeAdvert;
bool syncActive = false;

}  // namespace

BeginResult begin(const char* ip, uint16_t port, const char* token, const char* board, const Advert& advert) {
  if (!advert.packageId[0]) return BeginResult::NotAdvertised;
  if (!ip || !ip[0] || !port || strcmp(advert.packageId, LearningPack::PACKAGE_ID) != 0 ||
      advert.contentVersion == 0 || advert.formatVersion != LearningPack::FORMAT_VERSION ||
      advert.size < sizeof(LearningPack::Header) || advert.size > LearningPack::MAX_PACK_BYTES ||
      !validMd5(advert.md5) || !advert.licenseSpdx[0]) {
    AgentLog::line("LEARN", "pack advert rejected: id=%s version=%lu format=%u size=%lu", advert.packageId,
                   (unsigned long)advert.contentVersion, (unsigned)advert.formatVersion, (unsigned long)advert.size);
    return BeginResult::Failed;
  }

  LearningPack::Metadata installed{};
  if (LearningPack::ensureAvailable(&installed) && installed.contentVersion >= advert.contentVersion) {
    return BeginResult::Current;
  }
  if (!ensureDirectories()) return BeginResult::Failed;

  char requestPath[128];
  if (snprintf(requestPath, sizeof(requestPath), "/learning/pack?id=%s&version=%lu", LearningPack::PACKAGE_ID,
               (unsigned long)advert.contentVersion) >= (int)sizeof(requestPath) ||
      !SegmentedDownload::begin(ip, port, token, board, requestPath, LearningPack::PACK_TEMP_PATH, advert.size,
                                advert.md5)) {
    return BeginResult::Failed;
  }
  activeAdvert = advert;
  syncActive = true;
  return BeginResult::Started;
}

Step service() {
  if (!syncActive) return Step::Idle;
  const SegmentedDownload::Step step = SegmentedDownload::service();
  if (step == SegmentedDownload::Step::Progress) return Step::Progress;
  if (step == SegmentedDownload::Step::Retry) return Step::Retry;
  if (step != SegmentedDownload::Step::Complete) {
    syncActive = false;
    return Step::Failed;
  }
  LearningPack::Metadata metadata{};
  const bool accepted = LearningPack::validate(LearningPack::PACK_TEMP_PATH, &metadata) &&
                        metadata.contentVersion == activeAdvert.contentVersion &&
                        strcmp(metadata.packageId, activeAdvert.packageId) == 0 &&
                        strcmp(metadata.licenseSpdx, activeAdvert.licenseSpdx) == 0;
  if (!accepted) {
    AgentLog::line("LEARN", "pack validation failed after verified transfer");
    Storage.remove(LearningPack::PACK_TEMP_PATH);
    syncActive = false;
    return Step::Failed;
  }
  LearningPack::Metadata installed{};
  if (!LearningPack::install(LearningPack::PACK_TEMP_PATH, &installed)) {
    Storage.remove(LearningPack::PACK_TEMP_PATH);
    syncActive = false;
    return Step::Failed;
  }
  syncActive = false;
  return Step::Updated;
}

void cancel() {
  if (syncActive) SegmentedDownload::cancel();
  syncActive = false;
}

bool active() { return syncActive; }
uint32_t downloadedBytes() { return syncActive ? SegmentedDownload::downloadedBytes() : 0; }
uint32_t totalBytes() { return syncActive ? SegmentedDownload::totalBytes() : 0; }

}  // namespace LearningPackSync
}  // namespace PocketDaily
