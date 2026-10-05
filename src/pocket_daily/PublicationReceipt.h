#pragma once

#include <cstdint>

namespace PocketDaily::Publication {
// One durable receipt for the most recent publication. Matching is byte-exact;
// absence is unknown, never evidence that retrying publication is safe.
inline constexpr char RECEIPT_PATH[] = "/.crosspoint/pocket-publication.bin";
struct Request {
  const char* staging;
  const char* target;
  uint32_t size;
  uint32_t crc32;
};
bool valid(const Request& request);
bool matches(const Request& request);
bool save(const Request& request);
}  // namespace PocketDaily::Publication
