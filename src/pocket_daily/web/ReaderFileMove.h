#pragma once
#include <cstddef>

namespace PocketDaily::Web {
struct MoveStatePath {
  const char* source;
  const char* destination;
};
enum class MoveResult { Moved, Conflict, Failed, RollbackFailed };

// Single-request SD rename with companion state. No copies, overwrite or heap
// buffers. Roll back state if a later rename fails; report rollback failures.
template <typename StorageType>
MoveResult moveFileWithState(StorageType& storage, const char* source, const char* destination,
                             const MoveStatePath* state, size_t count) {
  if (count > 3 || storage.exists(destination)) return MoveResult::Conflict;
  for (size_t i = 0; i < count; ++i) {
    if (state[i].source[0] && storage.exists(state[i].destination)) return MoveResult::Conflict;
  }
  bool moved[3] = {};
  size_t completed = 0;
  for (; completed < count; ++completed) {
    const auto& item = state[completed];
    if (!item.source[0] || !storage.exists(item.source)) continue;
    if (!storage.rename(item.source, item.destination)) break;
    moved[completed] = true;
  }
  if (completed == count && storage.rename(source, destination)) return MoveResult::Moved;
  bool restored = true;
  while (completed > 0) {
    --completed;
    if (moved[completed] && !storage.rename(state[completed].destination, state[completed].source)) restored = false;
  }
  return restored ? MoveResult::Failed : MoveResult::RollbackFailed;
}
}  // namespace PocketDaily::Web
