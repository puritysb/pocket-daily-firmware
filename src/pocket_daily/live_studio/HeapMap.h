#pragma once

#include <cstddef>

// HN-2 evidence: a runtime heap map, serialized as text lines, readable over
// HTTP (dev builds). Names the buckets that consume the ~240 KiB the static
// analysis cannot see (WiFi driver, lwIP, task stacks, heap fragmentation).
#if defined(ENABLE_DEV_REMOTE_FLASH) || defined(ENABLE_HEAP_MAP)
#define POCKET_HEAP_MAP_ENABLED 1
#else
#define POCKET_HEAP_MAP_ENABLED 0
#endif

namespace PocketDaily::HeapMap {

// Renders into `out` (bounded); returns bytes written.
size_t render(char* out, size_t cap);

}  // namespace PocketDaily::HeapMap
