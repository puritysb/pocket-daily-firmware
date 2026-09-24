#pragma once
#include "ContentRevisionStore.h"

namespace PocketDaily::Content {
struct ActiveRevision {
  uint32_t generation = 0;
  char revision[65]{};
  RevisionInfo content;
};
enum class ActiveResult { Ok, NoActive, ReadFailed, VerifyFailed, WriteFailed, ReadbackFailed, GenerationExhausted };
struct RetiredRevision {
  char revision[65]{};
};
enum class RetirementResult { Ok, Protected, Unavailable, InvalidRevision, InvalidManifest, RemoveFailed };

// Conservative, bounded cleanup of one evicted published revision. Both active
// records must be readable and valid. Also preserves the caller's displayed
// revision (empty string means no displayed content; nullptr is unknown).
// Caller excludes activation, publication and presentation changes throughout.
// Deletes only manifest-listed files, never recursively deletes unknown files.
RetirementResult retireRevision(const char* revision, const char* displayedRevision, uint16_t capabilities,
                                void (*progress)() = nullptr);

// Read-only boot recovery: newest record whose entire revision validates.
ActiveResult recoverActiveRevision(uint16_t capabilities, ActiveRevision& active, void (*progress)() = nullptr);
// Caller must own immutable candidate/current directories and serialize all
// activation operations. The content HTTP handler gates live writers before
// calling this; generic mutations cannot write published content/active records.
// Repeats of the selected revision are write-free.
// A failed write/readback can still leave a complete new record; recover to
// resolve that ambiguity, never infer that failure means "nothing committed".
ActiveResult activateRevision(const char* revision, uint16_t capabilities, ActiveRevision& active,
                              void (*progress)() = nullptr, RetiredRevision* retired = nullptr);
}  // namespace PocketDaily::Content
