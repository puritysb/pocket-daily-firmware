#include "ContentViewState.h"

#include <cstring>

namespace PocketDaily::Content {
void ContentViewState::reset() {
  cards_.reset();
  revision_[0] = '\0';
  generation_ = 0;
}

ContentViewState::LoadResult ContentViewState::load(const char* expectedRevision, void (*progress)()) {
  // The owner holds display exclusion throughout. Reuse the allocation on
  // success, but release it on failure; publish metadata only after verification.
  // Keep revision_ intact during admission: expectedRevision may alias it.
  const auto fail = [this](LoadResult result) {
    reset();
    return result;
  };
  if (expectedRevision && !validRevision(expectedRevision)) return fail(LoadResult::InvalidTarget);
  ActiveRevision active;
  const auto recovered = recoverActiveRevision(SUPPORTED_CAPABILITIES, active, progress, &cards_);
  if (recovered == ActiveResult::NoActive) return fail(LoadResult::NoActive);
  if (recovered == ActiveResult::OutOfMemory) return fail(LoadResult::OutOfMemory);
  if (recovered != ActiveResult::Ok) return fail(LoadResult::Unavailable);
  if (expectedRevision && strcmp(active.revision, expectedRevision) != 0) return fail(LoadResult::TargetChanged);

  // Recovery already collected the cards under the same immutable-directory
  // ownership. Reopening and verifying the whole revision again adds no new
  // integrity guarantee; its pre/post hashes and semantic checks all ran above.
  memcpy(revision_, active.revision, sizeof(revision_));
  generation_ = active.generation;
  return cards_ ? LoadResult::Ready : LoadResult::Empty;
}
}  // namespace PocketDaily::Content
