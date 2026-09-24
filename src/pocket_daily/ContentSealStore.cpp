#include "ContentSealStore.h"

#include <DirectoryAccounting.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>

namespace PocketDaily::Content {
namespace {
bool inventoryMatches(const char* path, uint32_t files, uint64_t bytes, void (*progress)()) {
  // A revision is flat: exactly its verified assets plus the manifest. Bound
  // raw work too, including deleted and long-name slots, not just live files.
  DirectoryAccounting scan(Storage.filesystemFormat(), {512, files, 0, bytes});
  auto directory = Storage.open(path, O_RDONLY);
  if (!directory) return false;
  while (scan.state() == DirectoryAccounting::State::Reading) {
    scan.step(directory);
    if (progress) progress();  // Caller must never dispatch competing writers.
  }
  DirectoryAccounting::Totals totals;
  return scan.totals(totals) && totals.files == files && totals.bytes == bytes;
}

bool inventoryMatches(const char* path, const ManifestInfo& manifest, void (*progress)()) {
  const uint64_t bytes = manifest.contentBytes + MANIFEST_HEADER_BYTES +
                         static_cast<uint64_t>(manifest.fileCount) * MANIFEST_ENTRY_BYTES + 4;
  return inventoryMatches(path, static_cast<uint32_t>(manifest.fileCount) + 1, bytes, progress);
}

// An already-published retry can arrive with a complete duplicate prepared by
// the app. Missing assets may have been removed by an interrupted cleanup;
// every remaining file must be a verified duplicate. Unknown data is retained.
// The published revision remains authoritative even if cleanup fails.
void discardDuplicateStaging(const char* revision, uint16_t capabilities, void (*progress)()) {
  char staging[sizeof(CONTENT_STAGING_ROOT) + 65];
  snprintf(staging, sizeof(staging), "%s/%s", CONTENT_STAGING_ROOT, revision);
  if (!Storage.exists(staging)) return;
  PreparedRevision info;
  static_assert(sizeof(staging) + sizeof(info) + 32 < 256, "Duplicate cleanup stack budget");
  if (inspectStagedRevision(revision, capabilities, info, progress) != RevisionResult::Ok) return;
  // The manifest hash binds this subset to the already-verified published
  // revision. An unreadable/corrupt/undeclared file is NOT in the verified
  // subset and therefore makes exact inventory accounting fail closed.
  const uint64_t bytes = info.verifiedBytes + MANIFEST_HEADER_BYTES +
                         static_cast<uint64_t>(info.manifest.fileCount) * MANIFEST_ENTRY_BYTES + 4;
  if (!inventoryMatches(staging, static_cast<uint32_t>(info.verifiedCount) + 1, bytes, progress)) return;
  if (!removeDuplicateStagedRevisionFiles(revision, capabilities, progress))
    LOG_ERR("CONTENT", "Duplicate staging cleanup incomplete; published revision retained");
}

SealResult seal(const char* revision, uint16_t capabilities, RevisionInfo& info, void (*progress)()) {
  if (!validRevision(revision)) return SealResult::InvalidRevision;
  if (!Storage.ready()) return SealResult::Unavailable;
  char target[sizeof(CONTENT_ROOT) + 65];
  char staging[sizeof(CONTENT_STAGING_ROOT) + 65];
  static_assert(sizeof(target) + sizeof(staging) + sizeof(RevisionInfo) + 32 < 256, "Seal stack budget");
  snprintf(target, sizeof(target), "%s/%s", CONTENT_ROOT, revision);
  if (Storage.exists(target)) {
    if (verifyRevision(revision, capabilities, info, progress) != RevisionResult::Ok ||
        !inventoryMatches(target, info.manifest, progress))
      return SealResult::ExistingInvalid;
    discardDuplicateStaging(revision, capabilities, progress);
    return SealResult::Ok;
  }
  if (verifyStagedRevision(revision, capabilities, info, progress) != RevisionResult::Ok)
    return SealResult::InvalidCandidate;
  snprintf(staging, sizeof(staging), "%s/%s", CONTENT_STAGING_ROOT, revision);
  if (!inventoryMatches(staging, info.manifest, progress)) return SealResult::InvalidCandidate;
  if (!Storage.mkdir(CONTENT_ROOT) && !Storage.exists(CONTENT_ROOT)) return SealResult::MoveFailed;
  if (!Storage.rename(staging, target)) return SealResult::MoveFailed;
  return verifyRevision(revision, capabilities, info, progress) == RevisionResult::Ok &&
                 inventoryMatches(target, info.manifest, progress)
             ? SealResult::Ok
             : SealResult::ReadbackFailed;
}
}  // namespace

SealResult sealRevision(const char* revision, uint16_t capabilities, RevisionInfo& info, void (*progress)()) {
  info = {};
  const auto result = seal(revision, capabilities, info, progress);
  if (result != SealResult::Ok) {
    info = {};
    LOG_ERR("CONTENT", "Seal failed: %u", static_cast<unsigned>(result));
  }
  return result;
}
}  // namespace PocketDaily::Content
