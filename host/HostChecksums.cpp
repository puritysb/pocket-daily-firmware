#include <uzlib.h>
#include <zlib.h>

// The vendored inflater declares these optional checked-stream functions but
// firmware drops their unused section. The host library resolves them through zlib.
// uzlib_crc32 carries the unfinalized state; zlib carries the finalized state.
extern "C" uint32_t uzlib_adler32(const void* data, unsigned length, uint32_t previous) {
  return static_cast<uint32_t>(adler32(previous, static_cast<const Bytef*>(data), length));
}
extern "C" uint32_t uzlib_crc32(const void* data, unsigned length, uint32_t previous) {
  return ~static_cast<uint32_t>(crc32(~previous, static_cast<const Bytef*>(data), length));
}
