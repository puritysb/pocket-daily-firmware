#include "pocket_daily/learning_pack.h"

#include <HalStorage.h>
#include <mbedtls/sha256.h>

#include <cstdio>
#include <cstring>

#include "agent/AgentLog.h"

namespace PocketDaily {
namespace LearningPack {
namespace {

bool payloadHashValid(HalFile& file, const Header& header) {
  if (!file.seekSet(sizeof(Header))) return false;
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  if (mbedtls_sha256_starts(&sha, 0) != 0) {
    mbedtls_sha256_free(&sha);
    return false;
  }
  uint8_t buffer[1024];
  uint32_t remaining = header.totalBytes - sizeof(Header);
  while (remaining) {
    const size_t want = remaining > sizeof(buffer) ? sizeof(buffer) : remaining;
    const int got = file.read(buffer, want);
    if (got != static_cast<int>(want) || mbedtls_sha256_update(&sha, buffer, want) != 0) {
      mbedtls_sha256_free(&sha);
      return false;
    }
    remaining -= static_cast<uint32_t>(want);
  }
  uint8_t actual[32];
  const bool finished = mbedtls_sha256_finish(&sha, actual) == 0;
  mbedtls_sha256_free(&sha);
  return finished && memcmp(actual, header.payloadSha256, sizeof(actual)) == 0;
}

void copyMetadata(const Header& header, Metadata* metadata) {
  if (!metadata) return;
  *metadata = {};
  metadata->contentVersion = header.contentVersion;
  metadata->recordCount = header.recordCount;
  metadata->totalBytes = header.totalBytes;
  snprintf(metadata->packageId, sizeof(metadata->packageId), "%s", header.packageId);
  snprintf(metadata->locale, sizeof(metadata->locale), "%s", header.locale);
  snprintf(metadata->title, sizeof(metadata->title), "%s", header.title);
  snprintf(metadata->licenseSpdx, sizeof(metadata->licenseSpdx), "%s", header.licenseSpdx);
  snprintf(metadata->attribution, sizeof(metadata->attribution), "%s", header.attribution);
}

bool makeDirectories() {
  if (!Storage.exists("/pocket-daily") && !Storage.mkdir("/pocket-daily")) return false;
  return Storage.exists(PACK_DIR) || Storage.mkdir(PACK_DIR);
}

}  // namespace

bool validate(const char* path, Metadata* metadata) {
  if (metadata) *metadata = {};
  if (!Storage.ready() || !path || !path[0] || !Storage.exists(path)) return false;
  HalFile file = Storage.open(path, O_RDONLY);
  if (!file || file.size() < sizeof(Header) || file.size() > MAX_PACK_BYTES) return false;
  Header header{};
  if (file.read(&header, sizeof(header)) != static_cast<int>(sizeof(header)) || !headerValid(header, file.size()) ||
      !payloadHashValid(file, header)) {
    AgentLog::line("LEARN", "pack rejected: %s", path);
    return false;
  }
  copyMetadata(header, metadata);
  return true;
}

bool ensureAvailable(Metadata* metadata) {
  if (validate(PACK_PATH, metadata)) return true;
  if (!validate(PACK_BACKUP_PATH, metadata)) return false;
  Storage.remove(PACK_PATH);
  if (!Storage.rename(PACK_BACKUP_PATH, PACK_PATH)) return false;
  AgentLog::line("LEARN", "recovered learning pack backup");
  return validate(PACK_PATH, metadata);
}

bool install(const char* candidatePath, Metadata* metadata) {
  Metadata candidate{};
  if (!candidatePath || strcmp(candidatePath, PACK_PATH) == 0 || !validate(candidatePath, &candidate) ||
      !makeDirectories()) {
    return false;
  }
  Storage.remove(PACK_BACKUP_PATH);
  const bool hadActive = Storage.exists(PACK_PATH);
  if (hadActive && !Storage.rename(PACK_PATH, PACK_BACKUP_PATH)) return false;
  if (!Storage.rename(candidatePath, PACK_PATH)) {
    if (hadActive) Storage.rename(PACK_BACKUP_PATH, PACK_PATH);
    return false;
  }
  Metadata installed{};
  if (!validate(PACK_PATH, &installed)) {
    Storage.remove(PACK_PATH);
    if (hadActive) Storage.rename(PACK_BACKUP_PATH, PACK_PATH);
    return false;
  }
  Storage.remove(PACK_BACKUP_PATH);
  if (metadata) *metadata = installed;
  AgentLog::line("LEARN", "pack installed: %s v%lu records=%lu", installed.packageId,
                 (unsigned long)installed.contentVersion, (unsigned long)installed.recordCount);
  return true;
}

}  // namespace LearningPack
}  // namespace PocketDaily
