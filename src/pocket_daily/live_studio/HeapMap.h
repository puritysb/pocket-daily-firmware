#pragma once

#include <cstddef>

// HN-2 evidence: a runtime heap map, serialized as text lines, readable over
// HTTP (dev builds). Names the buckets that consume the ~240 KiB the static
// analysis cannot see (WiFi driver, lwIP, task stacks, heap fragmentation).
namespace PocketDaily::HeapMap {

// Renders into `out` (bounded); returns bytes written.
size_t render(char* out, size_t cap);

}  // namespace PocketDaily::HeapMap
