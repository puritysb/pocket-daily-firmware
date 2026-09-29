#include "ReadingProgress.h"

#include <ArduinoJson.h>

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "ContentChecksum.h"
#include "TextValidation.h"

namespace PocketDaily::ReadingProgress {
namespace {
constexpr uint8_t VERSION = 1;
constexpr char RECORD_MAGIC[4] = {'P', 'D', 'R', 'P'};
constexpr char OFFER_MAGIC[4] = {'P', 'D', 'R', 'O'};
constexpr char XPOINTER_PREFIX[] = "/body/DocFragment[";
constexpr size_t CRC_BYTES = 4;

uint32_t crc32(const uint8_t* bytes, const size_t size) { return ~Content::contentCrcUpdate(0xFFFFFFFFu, bytes, size); }

// Bounded little-endian writer/reader; memcpy only (RISC-V faults on unaligned loads).
class Writer {
 public:
  Writer(uint8_t* out, const size_t capacity) : out(out), capacity(capacity) {}
  void bytes(const void* data, const size_t size) {
    if (!ok || size > capacity - used) {
      ok = false;
      return;
    }
    memcpy(out + used, data, size);
    used += size;
  }
  void u8(const uint8_t v) { bytes(&v, 1); }
  void u16(const uint16_t v) {
    const uint8_t b[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
    bytes(b, 2);
  }
  void u32(const uint32_t v) {
    const uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
                          static_cast<uint8_t>(v >> 24)};
    bytes(b, 4);
  }
  void f32(const float v) { u32(std::bit_cast<uint32_t>(v)); }
  size_t finish() {
    if (!ok) return 0;
    u32(crc32(out, used));
    return ok ? used : 0;
  }

 private:
  uint8_t* out;
  size_t capacity;
  size_t used = 0;
  bool ok = true;
};

class Reader {
 public:
  Reader(const uint8_t* in, const size_t size) : in(in), size(size) {}
  bool bytes(void* data, const size_t count) {
    if (count > size - used) return false;
    memcpy(data, in + used, count);
    used += count;
    return true;
  }
  bool u8(uint8_t& v) { return bytes(&v, 1); }
  bool u16(uint16_t& v) {
    uint8_t b[2];
    if (!bytes(b, 2)) return false;
    v = static_cast<uint16_t>(b[0] | (b[1] << 8));
    return true;
  }
  bool u32(uint32_t& v) {
    uint8_t b[4];
    if (!bytes(b, 4)) return false;
    v = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) | (static_cast<uint32_t>(b[2]) << 16) |
        (static_cast<uint32_t>(b[3]) << 24);
    return true;
  }
  bool f32(float& v) {
    uint32_t bits = 0;
    if (!u32(bits)) return false;
    v = std::bit_cast<float>(bits);
    return true;
  }
  // Reads `length` bytes into a NUL-terminated buffer of `capacity`.
  bool text(char* out, const size_t length, const size_t capacity) {
    if (length >= capacity || !bytes(out, length)) return false;
    out[length] = '\0';
    return true;
  }
  size_t position() const { return used; }

 private:
  const uint8_t* in;
  size_t size;
  size_t used = 0;
};

// Checks size, trailing CRC, magic and version; leaves `reader` after the header. The byte
// after the version is returned in `flags` (records) and ignored by offers.
bool openRecord(const uint8_t* bytes, const size_t size, const char (&magic)[4], Reader& reader,
                uint8_t* flags = nullptr) {
  if (!bytes || size < 4 + 2 + CRC_BYTES) return false;
  const uint8_t* crcBytes = bytes + size - CRC_BYTES;
  const uint32_t stored = static_cast<uint32_t>(crcBytes[0]) | (static_cast<uint32_t>(crcBytes[1]) << 8) |
                          (static_cast<uint32_t>(crcBytes[2]) << 16) | (static_cast<uint32_t>(crcBytes[3]) << 24);
  if (stored != crc32(bytes, size - CRC_BYTES)) return false;
  char header[4];
  uint8_t version = 0;
  uint8_t reserved = 0;
  const bool ok = reader.bytes(header, 4) && memcmp(header, magic, 4) == 0 && reader.u8(version) &&
                  version == VERSION && reader.u8(reserved);
  if (ok && flags) *flags = reserved;
  return ok;
}

bool hexDigit(const char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }

// JSON string with the escapes RFC 8259 requires; UTF-8 passes through.
bool appendJsonString(const char* text, char* out, const size_t capacity, size_t& used) {
  auto put = [&](const char c) {
    if (used + 1 >= capacity) return false;
    out[used++] = c;
    return true;
  };
  if (!put('"')) return false;
  for (const char* p = text; *p; ++p) {
    const auto c = static_cast<unsigned char>(*p);
    if (c == '"' || c == '\\') {
      if (!put('\\') || !put(static_cast<char>(c))) return false;
    } else if (c < 0x20) {
      char escaped[7];
      snprintf(escaped, sizeof(escaped), "\\u%04x", c);
      for (const char* e = escaped; *e; ++e) {
        if (!put(*e)) return false;
      }
    } else if (!put(static_cast<char>(c))) {
      return false;
    }
  }
  return put('"');
}

bool appendRaw(const char* text, char* out, const size_t capacity, size_t& used) {
  const size_t length = strlen(text);
  if (length >= capacity - used) return false;
  memcpy(out + used, text, length);
  used += length;
  out[used] = '\0';
  return true;
}

bool copyText(const JsonVariantConst value, char* out, const size_t capacity, size_t& length) {
  if (!value.is<const char*>()) return false;
  const char* text = value.as<const char*>();
  length = strlen(text);
  if (length >= capacity) return false;
  memcpy(out, text, length + 1);
  return true;
}
}  // namespace

bool validDigest(const char* text) {
  if (!text || strlen(text) != DIGEST_HEX) return false;
  for (size_t i = 0; i < DIGEST_HEX; ++i) {
    if (!hexDigit(text[i])) return false;
  }
  return true;
}

bool validXPointer(const char* text, const size_t length) {
  constexpr size_t prefix = sizeof(XPOINTER_PREFIX) - 1;
  if (!text || length <= prefix || length > MAX_XPOINTER_BYTES || strncmp(text, XPOINTER_PREFIX, prefix) != 0) {
    return false;
  }
  for (size_t i = 0; i < length; ++i) {
    const auto c = static_cast<unsigned char>(text[i]);
    if (c < 0x21 || c > 0x7E) return false;  // XPointers are printable ASCII without spaces
  }
  return true;
}

bool validDevice(const char* text, const size_t length) {
  return text && length > 0 && length <= MAX_DEVICE_BYTES && Text::validUtf8(text, length, false);
}

bool validPercentage(const float value) { return std::isfinite(value) && value >= 0.0f && value <= 1.0f; }

bool isFurther(const float offered, const float current) { return offered > current + FURTHER_MARGIN; }

size_t encodeRecord(const Record& record, uint8_t* out, const size_t capacity) {
  const size_t documentLength = strlen(record.document);
  const size_t xpointerLength = strlen(record.xpointer);
  if (!validPercentage(record.percentage) || (documentLength && !validDigest(record.document)) ||
      (xpointerLength && !validXPointer(record.xpointer, xpointerLength))) {
    return 0;
  }
  Writer w(out, capacity);
  w.bytes(RECORD_MAGIC, 4);
  w.u8(VERSION);
  w.u8(record.flags);
  w.u16(record.spine);
  w.u16(record.page);
  w.u16(record.pageCount);
  w.f32(record.percentage);
  w.u32(record.seq);
  w.u32(record.updated);
  w.u32(record.fileSize);
  w.u8(static_cast<uint8_t>(documentLength));
  w.bytes(record.document, documentLength);
  w.u16(static_cast<uint16_t>(xpointerLength));
  w.bytes(record.xpointer, xpointerLength);
  return w.finish();
}

bool decodeRecord(const uint8_t* bytes, const size_t size, Record& record) {
  record = Record{};
  Reader r(bytes, size);
  uint8_t documentLength = 0;
  uint16_t xpointerLength = 0;
  Record decoded;
  const bool ok =
      openRecord(bytes, size, RECORD_MAGIC, r, &decoded.flags) && r.u16(decoded.spine) && r.u16(decoded.page) &&
      r.u16(decoded.pageCount) && r.f32(decoded.percentage) && r.u32(decoded.seq) && r.u32(decoded.updated) &&
      r.u32(decoded.fileSize) && r.u8(documentLength) && (documentLength == 0 || documentLength == DIGEST_HEX) &&
      r.text(decoded.document, documentLength, sizeof(decoded.document)) && r.u16(xpointerLength) &&
      r.text(decoded.xpointer, xpointerLength, sizeof(decoded.xpointer)) && r.position() + CRC_BYTES == size &&
      validPercentage(decoded.percentage) && (documentLength == 0 || validDigest(decoded.document)) &&
      (xpointerLength == 0 || validXPointer(decoded.xpointer, xpointerLength));
  if (ok) record = decoded;
  return ok;
}

size_t encodeOffer(const Offer& offer, uint8_t* out, const size_t capacity) {
  const size_t xpointerLength = strlen(offer.xpointer);
  const size_t deviceLength = strlen(offer.device);
  if (!validPercentage(offer.percentage) || !validXPointer(offer.xpointer, xpointerLength) ||
      !validDevice(offer.device, deviceLength)) {
    return 0;
  }
  Writer w(out, capacity);
  w.bytes(OFFER_MAGIC, 4);
  w.u8(VERSION);
  w.u8(0);
  w.f32(offer.percentage);
  w.u16(static_cast<uint16_t>(xpointerLength));
  w.bytes(offer.xpointer, xpointerLength);
  w.u8(static_cast<uint8_t>(deviceLength));
  w.bytes(offer.device, deviceLength);
  return w.finish();
}

bool decodeOffer(const uint8_t* bytes, const size_t size, Offer& offer) {
  offer = Offer{};
  Reader r(bytes, size);
  uint16_t xpointerLength = 0;
  uint8_t deviceLength = 0;
  Offer decoded;
  const bool ok = openRecord(bytes, size, OFFER_MAGIC, r) && r.f32(decoded.percentage) && r.u16(xpointerLength) &&
                  r.text(decoded.xpointer, xpointerLength, sizeof(decoded.xpointer)) && r.u8(deviceLength) &&
                  r.text(decoded.device, deviceLength, sizeof(decoded.device)) && r.position() + CRC_BYTES == size &&
                  validPercentage(decoded.percentage) && validXPointer(decoded.xpointer, xpointerLength) &&
                  validDevice(decoded.device, deviceLength);
  if (ok) offer = decoded;
  return ok;
}

bool parseOfferJson(const char* json, const size_t length, OfferRequest& out, const char*& error, bool& outOfMemory) {
  out = OfferRequest{};
  error = nullptr;
  outOfMemory = false;
  if (!json || length == 0 || length > MAX_OFFER_BODY_BYTES) {
    error = "Reading position body is missing or too large";
    return false;
  }
  JsonDocument doc;
  const DeserializationError parsed = deserializeJson(doc, json, length);
  if (parsed == DeserializationError::NoMemory) {
    outOfMemory = true;
    error = "Reader memory is too low for the reading position";
    return false;
  }
  if (parsed || !doc.is<JsonObjectConst>()) {
    error = "Reading position is not a JSON object";
    return false;
  }
  OfferRequest request;
  size_t n = 0;
  if (!copyText(doc["deviceID"], request.deviceID, sizeof(request.deviceID), n) || n != 8) {
    error = "Invalid deviceID";
    return false;
  }
  if (copyText(doc["document"], request.document, sizeof(request.document), n)) {
    for (size_t i = 0; i < n; ++i) {
      if (request.document[i] >= 'A' && request.document[i] <= 'F') request.document[i] += 'a' - 'A';
    }
  }
  if (!request.document[0] || !validDigest(request.document)) {
    error = "Invalid document digest";
    return false;
  }
  if (!copyText(doc["progress"], request.offer.xpointer, sizeof(request.offer.xpointer), n) ||
      !validXPointer(request.offer.xpointer, n)) {
    error = "Invalid progress XPointer";
    return false;
  }
  if (!copyText(doc["device"], request.offer.device, sizeof(request.offer.device), n) ||
      !validDevice(request.offer.device, n)) {
    error = "Invalid device name";
    return false;
  }
  const JsonVariantConst percentage = doc["percentage"];
  if (!percentage.is<float>() || !validPercentage(percentage.as<float>())) {
    error = "Invalid percentage";
    return false;
  }
  request.offer.percentage = percentage.as<float>();
  out = request;
  return true;
}

size_t writeListHead(const char* deviceId, char* out, const size_t capacity) {
  if (!out || capacity == 0) return 0;
  out[0] = '\0';
  size_t used = 0;
  if (!appendRaw("{\"v\":1,\"deviceID\":", out, capacity, used) || !appendJsonString(deviceId, out, capacity, used) ||
      !appendRaw(",\"books\":[", out, capacity, used)) {
    return 0;
  }
  return used;
}

size_t writeListEntry(const ListEntry& entry, char* out, const size_t capacity) {
  if (!out || capacity == 0 || !validPercentage(entry.percentage) || !validDigest(entry.document)) return 0;
  out[0] = '\0';
  size_t used = 0;
  char number[48];
  // The BLE list omits `path`: no file names cross the air.
  const bool opened = entry.path
                          ? appendRaw("{\"path\":", out, capacity, used) &&
                                appendJsonString(entry.path, out, capacity, used) && appendRaw(",", out, capacity, used)
                          : appendRaw("{", out, capacity, used);
  const bool ok = opened && appendRaw("\"document\":", out, capacity, used) &&
                  appendJsonString(entry.document, out, capacity, used) &&
                  appendRaw(",\"filenameDocument\":", out, capacity, used) &&
                  appendJsonString(entry.filenameDocument, out, capacity, used) &&
                  appendRaw(",\"progress\":", out, capacity, used) &&
                  (entry.xpointer[0] ? appendJsonString(entry.xpointer, out, capacity, used)
                                     : appendRaw("null", out, capacity, used)) &&
                  snprintf(number, sizeof(number), ",\"percentage\":%.5f", static_cast<double>(entry.percentage)) > 0 &&
                  appendRaw(number, out, capacity, used) &&
                  snprintf(number, sizeof(number), ",\"updated\":%lu,\"seq\":%lu}",
                           static_cast<unsigned long>(entry.updated), static_cast<unsigned long>(entry.seq)) > 0 &&
                  appendRaw(number, out, capacity, used);
  if (!ok) {
    out[0] = '\0';
    return 0;
  }
  return used;
}

size_t ListComposer::head(const char* deviceId, char* out, const size_t capacity) {
  books = 0;
  isFull = false;
  bytes = writeListHead(deviceId, out, capacity);
  return bytes;
}

size_t ListComposer::entry(const ListEntry& book, char* out, const size_t capacity) {
  if (isFull || !out || capacity < 2) return 0;
  if (books >= MAX_BOOKS) {
    isFull = true;
    return 0;
  }
  const size_t comma = books ? 1 : 0;
  out[0] = ',';
  const size_t length = writeListEntry(book, out + comma, capacity - comma);
  if (!length) return 0;  // e.g. a path too long to report
  if (bytes + comma + length + sizeof(LIST_TAIL) - 1 > MAX_LIST_BYTES) {
    isFull = true;
    return 0;
  }
  bytes += comma + length;
  books++;
  return comma + length;
}

size_t ListComposer::tail(char* out, const size_t capacity) {
  constexpr size_t length = sizeof(LIST_TAIL) - 1;
  if (!out || capacity < length) return 0;
  memcpy(out, LIST_TAIL, length);
  bytes += length;
  return length;
}
}  // namespace PocketDaily::ReadingProgress
