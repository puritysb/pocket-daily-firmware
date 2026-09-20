#pragma once

#include <cstddef>
#include <cstdint>

// HN-2 evidence: a runtime heap map streamed over HTTP in small chunks
// (the X3's File Transfer heap cannot afford a contiguous 4 KiB buffer, so
// the whole map renders through caller-provided ~512 B buffers).
#if defined(ENABLE_DEV_REMOTE_FLASH) || defined(ENABLE_HEAP_MAP)
#define POCKET_HEAP_MAP_ENABLED 1
#else
#define POCKET_HEAP_MAP_ENABLED 0
#endif

namespace PocketDaily::HeapMap {

// One line of capability totals per call into `out`.
size_t renderCaps(char* out, size_t cap);

// Task snapshot: query up to maxTasks into the caller's (stack) storage,
// then render batches of lines. Returns the number of tasks found.
size_t queryTasks(uintptr_t* tasks, size_t maxTasks);
size_t renderTaskBatch(const uintptr_t* tasks, size_t count, size_t from, size_t batch, char* out, size_t cap);

}  // namespace PocketDaily::HeapMap
