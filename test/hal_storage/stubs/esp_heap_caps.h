#pragma once
// Host stub: HalStorage::readFileToString probes the largest free block.
#include <cstddef>
#include <cstdint>
#define MALLOC_CAP_8BIT (1 << 2)
inline size_t heap_caps_get_largest_free_block(uint32_t) { return SIZE_MAX / 2; }
