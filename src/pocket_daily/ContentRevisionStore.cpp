#include "ContentRevisionStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <mbedtls/sha256.h>

#include <cstdio>
#include <cstring>

#include "ContentActiveStore.h"
#include "ContentCard.h"
#include "ContentImage.h"

namespace PocketDaily::Content {
namespace {
struct Workspace {
  ContentCard card;
  ManifestEntry entry;
  char path[160];
  char ids[CARD_CAP][sizeof(Card::cardId)];
  char images[CARD_CAP][64];
};
static_assert(sizeof(Workspace) < 1536, "Bound transient content verification allocation");
static_assert(sizeof(CONTENT_STAGING_ROOT) + 1 + 64 + 1 + 63 <= sizeof(Workspace::path), "Revision path capacity");

struct FileSource {
  HalFile& file;
  void (*progress)();
};
bool sourceRead(void* context, size_t offset, uint8_t* bytes, size_t count) {
  auto& source = *static_cast<FileSource*>(context);
  if (source.progress) source.progress();
  auto& file = source.file;
  return file.seek(offset) && file.read(bytes, count) == static_cast<int>(count);
}

bool matchesHash(HalFile& file, size_t expectedSize, const uint8_t* expected, void (*progress)() = nullptr) {
  if (file.size() != expectedSize || !file.seek(0)) return false;
  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);
  uint8_t buffer[64];
  uint8_t digest[32];
  static_assert(sizeof(context) + sizeof(buffer) + sizeof(digest) + 32 < 256, "Hash stack budget");
  bool valid = mbedtls_sha256_starts(&context, 0) == 0;
  for (size_t offset = 0; valid && offset < expectedSize;) {
    if (progress) progress();
    const size_t bytes = expectedSize - offset < sizeof(buffer) ? expectedSize - offset : sizeof(buffer);
    valid = file.read(buffer, bytes) == static_cast<int>(bytes) && mbedtls_sha256_update(&context, buffer, bytes) == 0;
    offset += bytes;
  }
  if (valid) valid = mbedtls_sha256_finish(&context, digest) == 0 && memcmp(digest, expected, sizeof(digest)) == 0;
  mbedtls_sha256_free(&context);
  return valid;
}

void filePath(Workspace& work, const char* root, const char* revision, const char* name) {
  snprintf(work.path, sizeof(work.path), "%s/%s/%s", root, revision, name);
}

bool copyChunks(HalFile& source, HalFile& destination, size_t size, void (*progress)()) {
  uint8_t buffer[64];
  if (!source.seek(0)) return false;
  for (size_t offset = 0; offset < size;) {
    if (progress) progress();
    const size_t count = size - offset < sizeof(buffer) ? size - offset : sizeof(buffer);
    if (source.read(buffer, count) != static_cast<int>(count) || destination.write(buffer, count) != count)
      return false;
    offset += count;
  }
  return true;
}

// Reuses the caller's path buffer. RAII closes the new candidate before the
// caller reopens and hashes it; interrupted copies remain unverified staging.
enum class ReuseResult { Skipped, Copied, Failed };
ReuseResult reuseFile(char (&path)[160], const ManifestEntry& entry, const char* revision, const char* reuseRevision,
                      void (*progress)()) {
  snprintf(path, sizeof(path), "%s/%s/%s", CONTENT_STAGING_ROOT, revision, entry.path);
  if (Storage.exists(path)) return ReuseResult::Skipped;
  snprintf(path, sizeof(path), "%s/%s/%s", CONTENT_ROOT, reuseRevision, entry.path);
  HalFile source = Storage.open(path, O_RDONLY);
  if (!source || !matchesHash(source, entry.bytes, entry.sha256, progress)) return ReuseResult::Skipped;
  snprintf(path, sizeof(path), "%s/%s/%s", CONTENT_STAGING_ROOT, revision, entry.path);
  HalFile destination = Storage.open(path, O_WRITE | O_CREAT | O_EXCL);
  // Never truncate an existing candidate, even on a false-missing read.
  if (!destination || !copyChunks(source, destination, entry.bytes, progress)) {
    LOG_ERR("CONTENT", "Candidate reuse copy failed");
    return ReuseResult::Failed;
  }
  return ReuseResult::Copied;
}

RevisionResult verifyFile(Workspace& work, const char* root, const char* revision, uint8_t& cards, void (*progress)()) {
  filePath(work, root, revision, work.entry.path);
  HalFile file = Storage.open(work.path, O_RDONLY);
  if (!file) return RevisionResult::MissingFile;
  if (!matchesHash(file, work.entry.bytes, work.entry.sha256, progress)) return RevisionResult::FileHash;
  FileSource reader{file, progress};
  const ManifestSource source{&reader, file.size(), sourceRead};
  switch (work.entry.kind) {
    case FileKind::Card:
      if (cards >= CARD_CAP || decodeContentCard(source, work.card) != CardResult::Ok)
        return RevisionResult::InvalidCard;
      for (uint8_t i = 0; i < cards; ++i) {
        if (strcmp(work.ids[i], work.card.card.cardId) == 0) return RevisionResult::DuplicateCard;
      }
      memcpy(work.ids[cards], work.card.card.cardId, sizeof(work.ids[cards]));
      memcpy(work.images[cards], work.card.imagePath, sizeof(work.images[cards]));
      ++cards;
      break;
    case FileKind::MonoImage: {
      ImageInfo info;
      if (validateContentImage(source, info) != ImageResult::Ok) return RevisionResult::InvalidImage;
      break;
    }
  }
  // Recheck after semantic reads as well. Immutable directory ownership is
  // still required; hashing does not itself exclude concurrent uploads.
  return matchesHash(file, work.entry.bytes, work.entry.sha256, progress) ? RevisionResult::Ok
                                                                          : RevisionResult::FileHash;
}

RevisionResult checkManifest(HalFile& manifest, const char* revision, uint16_t capabilities, ManifestInfo& info,
                             uint8_t (&expected)[32], void (*progress)() = nullptr) {
  for (unsigned i = 0; i < sizeof(expected); ++i) {
    const auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    expected[i] = static_cast<uint8_t>((nibble(revision[i * 2]) << 4) | nibble(revision[i * 2 + 1]));
  }
  const size_t size = manifest.size();
  if (size < 20 || size > 20 + MAX_FILES * MANIFEST_ENTRY_BYTES) return RevisionResult::InvalidManifest;
  if (!matchesHash(manifest, size, expected, progress)) return RevisionResult::ManifestHash;
  FileSource reader{manifest, progress};
  const ManifestSource source{&reader, size, sourceRead};
  if (validateManifest(source, capabilities, info) != ManifestResult::Ok) return RevisionResult::InvalidManifest;
  return RevisionResult::Ok;
}

RevisionResult verify(const char* root, const char* revision, uint16_t capabilities, RevisionInfo& info,
                      Workspace& work, void (*progress)(), RevisionCards* output) {
  filePath(work, root, revision, "manifest.pdcm");
  HalFile manifest = Storage.open(work.path, O_RDONLY);
  if (!manifest) return RevisionResult::MissingManifest;
  uint8_t expected[32];
  const auto manifestResult = checkManifest(manifest, revision, capabilities, info.manifest, expected, progress);
  if (manifestResult != RevisionResult::Ok) return manifestResult;
  const size_t size = manifest.size();
  FileSource reader{manifest, progress};
  const ManifestSource source{&reader, size, sourceRead};
  uint16_t imageMask = 0;
  bool hasLayout = false;
  for (uint16_t i = 0; i < info.manifest.fileCount; ++i) {
    if (!readManifestEntry(source, i, work.entry)) return RevisionResult::ReadFailed;
    if (work.entry.kind == FileKind::MonoImage) imageMask |= uint16_t(1u << i);
    const auto result = verifyFile(work, root, revision, info.cardCount, progress);
    if (result != RevisionResult::Ok) return result;
    if (work.entry.kind == FileKind::Card && work.card.layout != CardLayout::TextFirst) hasLayout = true;
    if (output && work.entry.kind == FileKind::Card) output->cards[info.cardCount - 1] = work.card;
  }
  if (hasLayout != bool(info.manifest.requiredCapabilities & CAP_CARD_LAYOUT)) return RevisionResult::InvalidManifest;
  uint16_t referenced = 0;
  for (uint8_t card = 0; card < info.cardCount; ++card) {
    if (!work.images[card][0]) continue;
    bool found = false;
    for (uint16_t i = 0; i < info.manifest.fileCount; ++i) {
      if (!readManifestEntry(source, i, work.entry)) return RevisionResult::ReadFailed;
      if (work.entry.kind == FileKind::MonoImage && strcmp(work.images[card], work.entry.path) == 0) {
        referenced |= uint16_t(1u << i);
        found = true;
        break;
      }
    }
    if (!found) return RevisionResult::MissingImage;
  }
  if (referenced != imageMask) return RevisionResult::UnusedImage;
  return matchesHash(manifest, size, expected, progress) ? RevisionResult::Ok : RevisionResult::ManifestHash;
}
}  // namespace

bool validRevision(const char* revision) {
  if (!revision || strnlen(revision, 65) != 64) return false;
  for (unsigned i = 0; i < 64; ++i) {
    const char c = revision[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

static RevisionResult verifyAtRoot(const char* root, const char* revision, uint16_t supportedCapabilities,
                                   RevisionInfo& info, void (*progress)(), RevisionCards* output = nullptr) {
  info = {};
  if (!validRevision(revision)) return RevisionResult::InvalidRevision;
  if (!Storage.ready()) return RevisionResult::Unavailable;
  // ~1.4 KiB, one temporary workspace. Reject stack (>256B) and permanent
  // allocation (would reduce radio headroom even outside content operations).
  auto work = makeUniqueNoThrow<Workspace>();
  if (!work) {
    LOG_ERR("CONTENT", "OOM allocating %uB revision workspace", static_cast<unsigned>(sizeof(Workspace)));
    return RevisionResult::OutOfMemory;
  }
  const auto result = verify(root, revision, supportedCapabilities, info, *work, progress, output);
  if (result != RevisionResult::Ok) {
    info = {};
    LOG_ERR("CONTENT", "Revision verification failed: %u", static_cast<unsigned>(result));
  }
  return result;
}
RevisionResult loadRevisionCards(const char* revision, uint16_t capabilities, RevisionCards& cards,
                                 void (*progress)()) {
  memset(&cards, 0, sizeof(cards));
  RevisionInfo info;
  const auto result = verifyAtRoot(CONTENT_ROOT, revision, capabilities, info, progress, &cards);
  if (result == RevisionResult::Ok) {
    cards.count = info.cardCount;
  } else {
    memset(&cards, 0, sizeof(cards));
  }
  return result;
}
RevisionResult verifyRevision(const char* revision, uint16_t supportedCapabilities, RevisionInfo& info,
                              void (*progress)()) {
  return verifyAtRoot(CONTENT_ROOT, revision, supportedCapabilities, info, progress);
}
RevisionResult verifyStagedRevision(const char* revision, uint16_t supportedCapabilities, RevisionInfo& info,
                                    void (*progress)()) {
  return verifyAtRoot(CONTENT_STAGING_ROOT, revision, supportedCapabilities, info, progress);
}

RevisionResult prepareStagedRevision(const char* revision, const char* reuseRevision, uint16_t capabilities,
                                     PreparedRevision& info, void (*progress)()) {
  info = {};
  if (!validRevision(revision)) return RevisionResult::InvalidRevision;
  if (reuseRevision && !validRevision(reuseRevision)) return RevisionResult::InvalidRevision;
  if (!Storage.ready()) return RevisionResult::Unavailable;
  struct Inspection {
    ManifestEntry entry;
    char path[160];
  };
  static_assert(sizeof(Inspection) <= 272, "Bound candidate inspection workspace");
  // <=272B exceeds stack budget; temporary, not an idle radio allocation.
  auto work = makeUniqueNoThrow<Inspection>();
  if (!work) {
    LOG_ERR("CONTENT", "OOM allocating %uB inspection workspace", static_cast<unsigned>(sizeof(Inspection)));
    return RevisionResult::OutOfMemory;
  }
  snprintf(work->path, sizeof(work->path), "%s/%s/manifest.pdcm", CONTENT_STAGING_ROOT, revision);
  HalFile manifest = Storage.open(work->path, O_RDONLY);
  if (!manifest) return RevisionResult::MissingManifest;
  uint8_t expected[32];
  PreparedRevision candidate;
  const auto result = checkManifest(manifest, revision, capabilities, candidate.manifest, expected, progress);
  if (result != RevisionResult::Ok) return result;
  FileSource reader{manifest, progress};
  const ManifestSource source{&reader, manifest.size(), sourceRead};
  for (uint16_t i = 0; i < candidate.manifest.fileCount; ++i) {
    if (!readManifestEntry(source, i, work->entry)) return RevisionResult::ReadFailed;
    const auto reused =
        reuseRevision ? reuseFile(work->path, work->entry, revision, reuseRevision, progress) : ReuseResult::Skipped;
    if (reused == ReuseResult::Failed) return RevisionResult::CopyFailed;
    snprintf(work->path, sizeof(work->path), "%s/%s/%s", CONTENT_STAGING_ROOT, revision, work->entry.path);
    HalFile file = Storage.open(work->path, O_RDONLY);
    if (file && matchesHash(file, work->entry.bytes, work->entry.sha256, progress)) {
      candidate.verifiedMask |= uint16_t(1u << i);
      ++candidate.verifiedCount;
      candidate.verifiedBytes += work->entry.bytes;  // validated manifest total <=256KiB
    } else if (reused == ReuseResult::Copied) {
      LOG_ERR("CONTENT", "Candidate reuse readback failed");
      return RevisionResult::CopyFailed;
    }
  }
  if (!matchesHash(manifest, source.size, expected, progress)) return RevisionResult::ManifestHash;
  info = candidate;
  return RevisionResult::Ok;
}
RevisionResult inspectStagedRevision(const char* revision, uint16_t capabilities, PreparedRevision& info,
                                     void (*progress)()) {
  return prepareStagedRevision(revision, nullptr, capabilities, info, progress);
}

namespace {
RetirementResult removeManifestFiles(const char* root, const char* revision, uint16_t capabilities,
                                     void (*progress)()) {
  if (!validRevision(revision)) return RetirementResult::InvalidRevision;
  struct Cleanup {
    ManifestEntry entry;
    char path[160];
  };
  static_assert(sizeof(Cleanup) <= 272, "Bound retirement workspace");
  // <=272B exceeds the stack budget; one temporary allocation, reused for all
  // <=16 files, with no permanent radio-session buffer or directory inventory.
  auto work = makeUniqueNoThrow<Cleanup>();
  if (!work) {
    LOG_ERR("CONTENT", "OOM allocating retirement workspace");
    return RetirementResult::Unavailable;
  }
  snprintf(work->path, sizeof(work->path), "%s/%s/manifest.pdcm", root, revision);
  HalFile manifest = Storage.open(work->path, O_RDONLY);
  if (!manifest) return RetirementResult::InvalidManifest;
  ManifestInfo info;
  uint8_t expected[32];
  if (checkManifest(manifest, revision, capabilities, info, expected, progress) != RevisionResult::Ok)
    return RetirementResult::InvalidManifest;
  FileSource reader{manifest, progress};
  const ManifestSource source{&reader, manifest.size(), sourceRead};
  for (uint16_t i = 0; i < info.fileCount; ++i) {
    if (!readManifestEntry(source, i, work->entry)) return RetirementResult::InvalidManifest;
    snprintf(work->path, sizeof(work->path), "%s/%s/%s", root, revision, work->entry.path);
    if (Storage.exists(work->path) && !Storage.remove(work->path)) return RetirementResult::RemoveFailed;
  }
  // Close before deleting the manifest. Retain it after any failed asset delete
  // so an explicit later retirement can resume without requiring intact assets.
  if (!manifest.close()) return RetirementResult::RemoveFailed;
  snprintf(work->path, sizeof(work->path), "%s/%s/manifest.pdcm", root, revision);
  if (!Storage.remove(work->path)) return RetirementResult::RemoveFailed;
  snprintf(work->path, sizeof(work->path), "%s/%s", root, revision);
  // Non-recursive: unexpected user files/subdirectories are never removed.
  return Storage.rmdir(work->path) ? RetirementResult::Ok : RetirementResult::RemoveFailed;
}
}  // namespace

RetirementResult removeRetiredRevisionFiles(const char* revision, uint16_t capabilities, void (*progress)()) {
  return removeManifestFiles(CONTENT_ROOT, revision, capabilities, progress);
}

bool removeDuplicateStagedRevisionFiles(const char* revision, uint16_t capabilities, void (*progress)()) {
  return removeManifestFiles(CONTENT_STAGING_ROOT, revision, capabilities, progress) == RetirementResult::Ok;
}
}  // namespace PocketDaily::Content
