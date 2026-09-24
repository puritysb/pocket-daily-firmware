#pragma once
#include <cstdint>
#include <string_view>

namespace PocketDaily::Content {
// Generic external file writers must never mutate published revisions or
// activation records, including their ancestors. Pass the full decoded path
// with its length, before any storage operation. Staging is a separate tree.
bool genericContentWriteAllowed(std::string_view path);
bool isContentStagingPath(std::string_view path);
// Per-file ingress bound, not total directory quota or free-space reservation.
// Call before every unknown-length chunk, counting any resumed prefix.
bool contentStagingRangeAllowed(uint64_t offset, uint64_t bytes);
bool contentWriteWithinBudget(std::string_view path, uint64_t offset, uint64_t bytes);
}  // namespace PocketDaily::Content
