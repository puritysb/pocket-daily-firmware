#pragma once

#include <cstddef>
#include <cstdint>

// The last EPUB section build that failed, kept in one small SD file so a
// failure the reader only showed as "Failed to index" can be read back over
// Wi-Fi (/api/status `lastBuildError`, docs/build-failure-log.md) without USB.
// Holds no book text: the book is identified by the hash that names its cache
// directory (/.crosspoint/epub_<book>).
namespace PocketDaily::BuildFailureLog {
inline constexpr char PATH[] = "/.crosspoint/last-build-error.bin";
inline constexpr size_t VERSION_BYTES = 64;
inline constexpr size_t RECORD_BYTES = 96;

// Mirrors Section::BuildStep; checked where both are visible (EpubReaderActivity).
enum Step : uint8_t {
  STEP_NONE = 0,
  STEP_HTML_STREAM,
  STEP_SECTION_FILE,
  STEP_BUILD_CONTEXT,
  STEP_PARSER,
  STEP_BEGIN_PARSE,
  STEP_LAYOUT,
  STEP_COMMIT,
  STEP_COUNT,
};

struct Entry {
  uint32_t book = 0;   // std::hash of the book path, as in its cache directory name
  uint16_t spine = 0;  // spine index that failed to build
  uint8_t step = 0;    // Step
  uint8_t detail = 0;  // HTML stream: ZipFile::StreamError; layout: 1 = out of memory
  uint16_t count = 0;  // consecutive failures of this book and spine
  uint32_t freeHeap = 0;
  uint32_t largestBlock = 0;
  uint32_t uptimeSec = 0;
  char version[VERSION_BYTES] = {};  // firmware that recorded it, NUL-terminated
};

// Fixed little-endian record with a CRC-32 trailer; false/0 on bad input.
size_t encode(const Entry& entry, uint8_t* out, size_t capacity);
bool decode(const uint8_t* bytes, size_t size, Entry& entry);

const char* stepName(uint8_t step);
// Detail name for the step, or nullptr when the detail carries no meaning.
const char* detailName(uint8_t step, uint8_t detail);

bool load(Entry& entry);
// Stores `entry`, setting its count from a repeat of the previously recorded book and spine.
// Temporary file + rename: a torn write never replaces a readable record.
bool record(Entry& entry);
}  // namespace PocketDaily::BuildFailureLog
