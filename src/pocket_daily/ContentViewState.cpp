#include "ContentViewState.h"

#include <Logging.h>
#include <Memory.h>

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
  const auto recovered = recoverActiveRevision(SUPPORTED_CAPABILITIES, active, progress);
  if (recovered == ActiveResult::NoActive) return fail(LoadResult::NoActive);
  if (recovered != ActiveResult::Ok) return fail(LoadResult::Unavailable);
  if (expectedRevision && strcmp(active.revision, expectedRevision) != 0) return fail(LoadResult::TargetChanged);

  if (active.content.cardCount) {
    // <=2400B is too large for the stack. One on-demand snapshot, reused on
    // reload; no permanent pool or overlapping old/new display allocation.
    static_assert(sizeof(RevisionCards) <= 2400, "Bound content view snapshot");
    if (!cards_) cards_ = makeUniqueNoThrow<RevisionCards>();
    if (!cards_) {
      LOG_ERR("CONTENT", "OOM allocating content view snapshot");
      return fail(LoadResult::OutOfMemory);
    }
    if (loadRevisionCards(active.revision, SUPPORTED_CAPABILITIES, *cards_, progress) != RevisionResult::Ok)
      return fail(LoadResult::Unavailable);
  } else {
    cards_.reset();
  }
  memcpy(revision_, active.revision, sizeof(revision_));
  generation_ = active.generation;
  return cards_ ? LoadResult::Ready : LoadResult::Empty;
}
}  // namespace PocketDaily::Content
