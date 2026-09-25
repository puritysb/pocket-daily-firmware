#pragma once
#include "ContentCard.h"
#include "ContentManifest.h"

namespace PocketDaily::Content {
enum class RetirementResult;
inline constexpr char CONTENT_ROOT[] = "/pocket-daily/content";
inline constexpr char CONTENT_STAGING_ROOT[] = "/pocket-daily/content-staging";
enum class RevisionResult {
  Ok,
  InvalidRevision,
  Unavailable,
  OutOfMemory,
  MissingManifest,
  ManifestHash,
  InvalidManifest,
  ReadFailed,
  MissingFile,
  FileHash,
  InvalidCard,
  InvalidImage,
  DuplicateCard,
  MissingImage,
  UnusedImage,
  CopyFailed
};
struct RevisionInfo {
  ManifestInfo manifest;
  uint8_t cardCount = 0;
};
struct PreparedRevision {
  ManifestInfo manifest;
  uint16_t verifiedMask = 0;  // canonical manifest entry order, bit0 = first file
  uint16_t verifiedCount = 0;
  uint32_t verifiedBytes = 0;  // logical bytes of the verified subset, not free space
};
struct RevisionCards {
  ContentCard cards[CARD_CAP]{};
  uint8_t count = 0;
};
// Caller-owned, unpublished output. Failure clears every card; never expose a
// partial revision. Uses the same full verification as activation, not a second
// unverified file read after verification. Caller owns directory exclusion.
RevisionResult loadRevisionCards(const char* revision, uint16_t capabilities, RevisionCards& cards,
                                 void (*progress)() = nullptr);
// Read-only, partial candidate inspection. Missing/mismatched/unreadable assets
// leave bits clear; only a fully verified manifest can produce a receipt.
// progress may feed the watchdog, but must never dispatch other file writers.
RevisionResult inspectStagedRevision(const char* revision, uint16_t capabilities, PreparedRevision& info,
                                     void (*progress)() = nullptr);
// Best-effort reuse from one pinned published revision. Creates only missing
// candidate files, never changes published data or activation. Caller excludes
// writers throughout. Receipt still certifies actual candidate bytes only.
RevisionResult prepareStagedRevision(const char* revision, const char* reuseRevision, uint16_t capabilities,
                                     PreparedRevision& info, void (*progress)() = nullptr);
bool validRevision(const char* revision);
// Path of one file of a published revision for read-only access: `manifest.pdcm`
// or a manifest-shaped card/image leaf name, never anything outside
// <CONTENT_ROOT>/<revision>/. On failure `out` is empty.
bool publishedFilePath(const char* revision, const char* name, char* out, size_t outSize);
// Room for the longest published path: root, 64-hex revision, 63-byte leaf name.
inline constexpr size_t PUBLISHED_PATH_BYTES = sizeof(CONTENT_ROOT) + 1 + 64 + 1 + 63 + 1;
// Read-only verification of <CONTENT_ROOT>/<64hex>/manifest.pdcm and all files.
// Caller must prevent writes to that directory for the entire verification and
// subsequent commit. No activation, deletion, network or success receipt here.
RevisionResult verifyRevision(const char* revision, uint16_t supportedCapabilities, RevisionInfo& info,
                              void (*progress)() = nullptr);
RevisionResult verifyStagedRevision(const char* revision, uint16_t supportedCapabilities, RevisionInfo& info,
                                    void (*progress)() = nullptr);
// Internal deletion primitive: only call after ContentActiveStore's retirement
// guards, under the same exclusive ownership. Not a network operation.
RetirementResult removeRetiredRevisionFiles(const char* revision, uint16_t capabilities, void (*progress)());
// Internal seal-only cleanup: caller must first fully verify BOTH the published
// revision and every remaining staged asset, including exact flat inventories,
// and retain exclusive writer ownership through deletion. Never recursive.
bool removeDuplicateStagedRevisionFiles(const char* revision, uint16_t capabilities, void (*progress)());
}  // namespace PocketDaily::Content
