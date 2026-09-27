#pragma once

#include <cstddef>
#include <cstdint>

// Reading-progress v1 (docs/reading-progress-v1.md): the reader reports where
// each recent EPUB was left, and accepts a position another device offers,
// which it only asks about the next time that book opens. Pure model, codecs
// and JSON; SD storage lives in ReadingProgressStore, the reader side in
// ReadingProgressReader.
namespace PocketDaily::ReadingProgress {
inline constexpr size_t MAX_XPOINTER_BYTES = 512;
inline constexpr size_t MAX_DEVICE_BYTES = 64;
inline constexpr size_t DIGEST_HEX = 32;
inline constexpr size_t MAX_BOOKS = 10;
// The app rejects a larger list (ReaderReadingList.maximumBytes).
inline constexpr size_t MAX_LIST_BYTES = 8 * 1024;
inline constexpr size_t MAX_OFFER_BODY_BYTES = 1024;
// An offer at least this far ahead of the current page is asked about; the app
// uses the same margin before it offers a position (ReadingSync.exchange).
inline constexpr float FURTHER_MARGIN = 0.004f;

// What the reader last recorded for one book (<cache>/pocket-reading.bin).
// ~570 B: allocate on the heap, never on a task stack.
struct Record {
  uint16_t spine = 0;
  uint16_t page = 0;
  uint16_t pageCount = 0;
  float percentage = 0.0f;                     // 0..1, start of the page
  uint32_t seq = 0;                            // per-reader counter, advances on every new record
  uint32_t updated = 0;                        // epoch seconds; 0 when the clock was not trustworthy
  uint32_t fileSize = 0;                       // the book size `document` was computed for
  char document[DIGEST_HEX + 1] = {};          // KOReader partial MD5, "" if unknown
  char xpointer[MAX_XPOINTER_BYTES + 1] = {};  // "" if it could not be computed
};

// A position another device offered (<cache>/pocket-reading-offer.bin).
struct Offer {
  float percentage = 0.0f;
  char xpointer[MAX_XPOINTER_BYTES + 1] = {};
  char device[MAX_DEVICE_BYTES + 1] = {};
};

// POST /api/pocket/v1/reading body.
struct OfferRequest {
  char deviceID[9] = {};
  char document[DIGEST_HEX + 1] = {};
  Offer offer;
};

bool validDigest(const char* text);
// "/body/DocFragment[N]..." in printable ASCII, at most MAX_XPOINTER_BYTES.
bool validXPointer(const char* text, size_t length);
// Non-empty strict UTF-8 without control characters, at most MAX_DEVICE_BYTES.
bool validDevice(const char* text, size_t length);
bool validPercentage(float value);
bool isFurther(float offered, float current);

// Little-endian fixed headers ("PDRP"/"PDRO", version 1) plus the strings and a
// CRC32; independent of struct padding. Decoders validate every field.
// Header 6 + fixed fields 22 + digest 1+32 + strings with length prefixes + CRC 4.
inline constexpr size_t MAX_RECORD_BYTES = 6 + 22 + 1 + DIGEST_HEX + 2 + MAX_XPOINTER_BYTES + 4;
inline constexpr size_t MAX_OFFER_BYTES = 6 + 4 + 2 + MAX_XPOINTER_BYTES + 1 + MAX_DEVICE_BYTES + 4;
size_t encodeRecord(const Record& record, uint8_t* out, size_t capacity);
bool decodeRecord(const uint8_t* bytes, size_t size, Record& record);
size_t encodeOffer(const Offer& offer, uint8_t* out, size_t capacity);
bool decodeOffer(const uint8_t* bytes, size_t size, Offer& offer);

// Strict body parser: every field present with the right type and valid value;
// unknown keys are ignored. `error` receives a short static reason.
bool parseOfferJson(const char* json, size_t length, OfferRequest& out, const char*& error, bool& outOfMemory);

// One list entry; `progress` null when `xpointer` is empty. Returns the bytes
// written (no terminator), or 0 when it does not fit.
struct ListEntry {
  const char* path = "";
  const char* document = "";
  const char* filenameDocument = "";
  const char* xpointer = "";
  float percentage = 0.0f;
  uint32_t updated = 0;
  uint32_t seq = 0;
};
size_t writeListEntry(const ListEntry& entry, char* out, size_t capacity);
// `{"v":1,"deviceID":"…","books":[` and `]}`.
size_t writeListHead(const char* deviceId, char* out, size_t capacity);
inline constexpr char LIST_TAIL[] = "]}";
}  // namespace PocketDaily::ReadingProgress
