#pragma once

#include <memory>

#include "ContentActiveStore.h"

namespace PocketDaily::Content {
// A display model, not an Activity: no radio, task, provider or framebuffer.
// The owner serializes load/reset with rendering and excludes storage writers.
class ContentViewState {
 public:
  enum class LoadResult { Ready, Empty, NoActive, InvalidTarget, TargetChanged, Unavailable, OutOfMemory };

  // expectedRevision is optional for offline startup, required by an eventual
  // live presentation request. Never silently present a recovered older target
  // in response to a request for a specific revision. No activation or writes.
  LoadResult load(const char* expectedRevision = nullptr, void (*progress)() = nullptr);
  void reset();
  const RevisionCards* cards() const { return cards_.get(); }
  const char* revision() const { return revision_; }
  uint32_t generation() const { return generation_; }

 private:
  std::unique_ptr<RevisionCards> cards_;
  char revision_[65]{};
  uint32_t generation_ = 0;
};
}  // namespace PocketDaily::Content
