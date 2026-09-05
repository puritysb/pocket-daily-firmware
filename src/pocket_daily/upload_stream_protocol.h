#pragma once

#include <cstddef>
#include <cstdint>

// Pocket Nearby Sync upload stream (TCP port advertised as `uploadStreamPort`).
//
// This header is deliberately free of Arduino types so the wire grammar can be
// exercised by the host test suite. The reader keeps every record on one
// long-lived socket, avoiding the per-request lwIP TIME_WAIT churn that the
// no-PSRAM X3 cannot afford during a multi-megabyte firmware transfer.
//
// Request header (ASCII, LF separated, terminated by one empty line):
//
//   POCKET-PUT/1
//   Path: /.pocket-<uuid>.part
//   Size: <decimal byte count>
//   Resume: 1                     (optional)
//
// Unknown "Key: value" lines are ignored for forward compatibility. `Path`
// must name a hidden staging file (`.pocket-*.part`) so a dropped link can
// never leave a partial user-visible book or update image behind.
//
// Replies (one ASCII line each):
//
//   RESUME <received>             only for `Resume: 1`, sent before any payload
//                                 byte is consumed. `received` is the verified
//                                 prefix the reader still holds for this
//                                 path/size pair (0 restarts from scratch).
//   OK <received> <CRC32 hex>     after the final payload byte was flushed.
//   ERROR <message>               at any time; the socket closes afterwards.
namespace PocketDaily::UploadStream {

constexpr const char* MAGIC = "POCKET-PUT/1";
constexpr size_t MAX_HEADER_BYTES = 320;
constexpr size_t MAX_PATH_BYTES = 256;
constexpr uint32_t CRC32_INITIAL = 0xFFFFFFFFU;

struct Request {
  char path[MAX_PATH_BYTES] = {};
  uint32_t size = 0;
  bool resume = false;
};

enum class HeaderStatus : uint8_t { Complete, Incomplete, TooLarge };

// Reports whether `buffer[0, length)` already contains the blank-line
// terminator. `capacity` bounds how much header the caller is willing to
// retain; a header that fills it without terminating is rejected.
HeaderStatus headerStatus(const char* buffer, size_t length, size_t capacity);

// Parses one complete header. Returns false for a malformed record, a
// duplicate or missing required key, a size that does not fit 32 bits, or a
// path that is not an acceptable hidden staging file.
bool parseHeader(const char* header, size_t length, Request& out);

// `/dir/.pocket-<id>.part`: absolute, no "..", no CR/LF, no empty component,
// no trailing slash, hidden Pocket staging name.
bool isStagingPath(const char* path);

uint32_t updateCrc32(uint32_t crc, const uint8_t* data, size_t length);
inline uint32_t finalizeCrc32(const uint32_t crc) { return crc ^ 0xFFFFFFFFU; }

// Reply formatters return the number of bytes written (excluding the NUL
// terminator) or 0 when `outSize` is too small.
size_t formatOkReply(char* out, size_t outSize, uint32_t received, uint32_t finalizedCrc32);
size_t formatResumeReply(char* out, size_t outSize, uint32_t received);
size_t formatErrorReply(char* out, size_t outSize, const char* message);

}  // namespace PocketDaily::UploadStream
