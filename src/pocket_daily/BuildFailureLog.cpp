#include "BuildFailureLog.h"

#include <HalStorage.h>

#include <cstring>

namespace PocketDaily::BuildFailureLog {
namespace {
constexpr uint8_t MAGIC[4] = {'P', 'D', 'B', 'F'};
constexpr uint8_t FORMAT_VERSION = 1;
constexpr size_t CRC_OFFSET = RECORD_BYTES - 4;
constexpr char TEMP_PATH[] = "/.crosspoint/last-build-error.tmp";

// Must match ZipFile::StreamError (checked in EpubReaderActivity).
constexpr const char* STREAM_ERRORS[] = {nullptr,  "open",    "entry", "read-buffer", "output-buffer",
                                         "window", "inflate", "read",  "write",       "method"};
constexpr const char* STEPS[] = {"none",   "html-stream", "section-file", "build-context",
                                 "parser", "begin-parse", "layout",       "commit"};
static_assert(sizeof(STEPS) / sizeof(STEPS[0]) == STEP_COUNT, "one name per step");

uint32_t crc32(const uint8_t* data, const size_t size) {
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
  }
  return ~crc;
}

void put16(uint8_t* out, const uint16_t value) {
  out[0] = static_cast<uint8_t>(value);
  out[1] = static_cast<uint8_t>(value >> 8);
}
void put32(uint8_t* out, const uint32_t value) {
  for (int i = 0; i < 4; ++i) out[i] = static_cast<uint8_t>(value >> (8 * i));
}
uint16_t get16(const uint8_t* in) { return static_cast<uint16_t>(in[0] | (in[1] << 8)); }
uint32_t get32(const uint8_t* in) {
  return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) | (static_cast<uint32_t>(in[2]) << 16) |
         (static_cast<uint32_t>(in[3]) << 24);
}
}  // namespace

size_t encode(const Entry& entry, uint8_t* out, const size_t capacity) {
  if (!out || capacity < RECORD_BYTES || entry.step >= STEP_COUNT) return 0;
  memset(out, 0, RECORD_BYTES);
  memcpy(out, MAGIC, sizeof(MAGIC));
  out[4] = FORMAT_VERSION;
  out[5] = entry.step;
  out[6] = entry.detail;
  put16(out + 8, entry.spine);
  put16(out + 10, entry.count);
  put32(out + 12, entry.book);
  put32(out + 16, entry.freeHeap);
  put32(out + 20, entry.largestBlock);
  put32(out + 24, entry.uptimeSec);
  // Always NUL-terminated inside the record, even for an unterminated input.
  memcpy(out + 28, entry.version, strnlen(entry.version, VERSION_BYTES - 1));
  put32(out + CRC_OFFSET, crc32(out, CRC_OFFSET));
  return RECORD_BYTES;
}

bool decode(const uint8_t* bytes, const size_t size, Entry& entry) {
  if (!bytes || size != RECORD_BYTES || memcmp(bytes, MAGIC, sizeof(MAGIC)) != 0 || bytes[4] != FORMAT_VERSION ||
      bytes[5] >= STEP_COUNT || bytes[28 + VERSION_BYTES - 1] != '\0' ||
      get32(bytes + CRC_OFFSET) != crc32(bytes, CRC_OFFSET)) {
    return false;
  }
  Entry decoded;
  decoded.step = bytes[5];
  decoded.detail = bytes[6];
  decoded.spine = get16(bytes + 8);
  decoded.count = get16(bytes + 10);
  decoded.book = get32(bytes + 12);
  decoded.freeHeap = get32(bytes + 16);
  decoded.largestBlock = get32(bytes + 20);
  decoded.uptimeSec = get32(bytes + 24);
  memcpy(decoded.version, bytes + 28, VERSION_BYTES);
  entry = decoded;
  return true;
}

const char* stepName(const uint8_t step) { return step < STEP_COUNT ? STEPS[step] : "unknown"; }

const char* detailName(const uint8_t step, const uint8_t detail) {
  if (step == STEP_HTML_STREAM && detail < sizeof(STREAM_ERRORS) / sizeof(STREAM_ERRORS[0])) {
    return STREAM_ERRORS[detail];
  }
  if (step == STEP_LAYOUT && detail == 1) return "out-of-memory";
  return nullptr;
}

bool load(Entry& entry) {
  HalFile file = Storage.open(PATH, O_RDONLY);
  if (!file) return false;
  uint8_t bytes[RECORD_BYTES];
  const bool read = file.size() == RECORD_BYTES && file.read(bytes, RECORD_BYTES) == static_cast<int>(RECORD_BYTES);
  file.close();
  return read && decode(bytes, RECORD_BYTES, entry);
}

bool record(Entry& entry) {
  Entry previous;
  const bool repeat = load(previous) && previous.book == entry.book && previous.spine == entry.spine;
  entry.count = repeat && previous.count < UINT16_MAX ? static_cast<uint16_t>(previous.count + 1) : 1;

  uint8_t bytes[RECORD_BYTES];
  if (encode(entry, bytes, sizeof(bytes)) != RECORD_BYTES) return false;
  {
    HalFile out = Storage.open(TEMP_PATH, O_WRITE | O_CREAT | O_TRUNC);
    if (!out || out.write(bytes, RECORD_BYTES) != RECORD_BYTES) {
      if (out) out.close();
      Storage.remove(TEMP_PATH);
      return false;
    }
    out.close();  // SdFat must not rename a path that is still open.
  }
  Storage.remove(PATH);
  return Storage.rename(TEMP_PATH, PATH);
}
}  // namespace PocketDaily::BuildFailureLog
