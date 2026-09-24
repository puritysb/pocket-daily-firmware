#pragma once
#include "ContentRevisionStore.h"

namespace PocketDaily::Content {
enum class SealResult {
  Ok,
  InvalidRevision,
  Unavailable,
  InvalidCandidate,
  ExistingInvalid,
  MoveFailed,
  ReadbackFailed
};
// Caller must exclusively own staging and serialize all seal/activate operations.
// Moves a verified staging directory to the immutable published tree. Never
// replaces an existing published directory and never changes active records.
// A failed move/readback is ambiguous: retry inspects the published revision.
// An already-published success may reclaim an exact, fully verified duplicate
// staging tree. Cleanup failure does not invalidate the published success.
SealResult sealRevision(const char* revision, uint16_t capabilities, RevisionInfo& info, void (*progress)() = nullptr);
}  // namespace PocketDaily::Content
