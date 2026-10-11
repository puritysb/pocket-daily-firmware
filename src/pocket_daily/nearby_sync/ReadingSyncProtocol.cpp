#include "ReadingSyncProtocol.h"

#include <cstdio>
#include <cstring>

#include "pocket_daily/ContentChecksum.h"

namespace Pocket::NearbySync {
namespace {
struct Field {
  const char* text = nullptr;
  size_t length = 0;
};

// Splits off the next space-terminated field; exactly one separator. With
// `restIsChunk` the text after the separator is a W chunk, which may itself
// begin with a space.
bool nextField(const char*& cursor, const char* end, Field& field, const bool restIsChunk = false) {
  if (cursor >= end || *cursor == ' ') return false;
  const char* start = cursor;
  while (cursor < end && *cursor != ' ') ++cursor;
  field.text = start;
  field.length = static_cast<size_t>(cursor - start);
  if (cursor < end) {
    ++cursor;                                                             // the separator
    if (cursor >= end || (*cursor == ' ' && !restIsChunk)) return false;  // trailing or doubled space
  }
  return true;
}

bool fieldIs(const Field& field, const char* word) {
  const size_t n = strlen(word);
  return field.length == n && memcmp(field.text, word, n) == 0;
}

// Unsigned decimal without sign or leading zeros (except "0"), <= 9 digits.
bool parseDecimal(const Field& field, uint32_t& value) {
  if (field.length == 0 || field.length > 9 || (field.length > 1 && field.text[0] == '0')) return false;
  uint32_t result = 0;
  for (size_t i = 0; i < field.length; ++i) {
    const char c = field.text[i];
    if (c < '0' || c > '9') return false;
    result = result * 10 + static_cast<uint32_t>(c - '0');
  }
  value = result;
  return true;
}

int hexValue(const char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

bool parseHex8(const Field& field, uint32_t& value) {
  if (field.length != 8) return false;
  uint32_t result = 0;
  for (size_t i = 0; i < 8; ++i) {
    const int nibble = hexValue(field.text[i]);
    if (nibble < 0) return false;
    result = (result << 4) | static_cast<uint32_t>(nibble);
  }
  value = result;
  return true;
}

size_t finishRecord(const int written, char* out, const size_t capacity) {
  if (written <= 0 || static_cast<size_t>(written) >= capacity || static_cast<size_t>(written) > RECORD_LIMIT) {
    if (out && capacity) out[0] = '\0';
    return 0;
  }
  return static_cast<size_t>(written);
}

bool usableId(const char* requestId) { return requestId && validRequestId(requestId, strlen(requestId)); }
}  // namespace

bool validRequestId(const char* text, const size_t length) {
  if (!text || length != REQUEST_ID_LENGTH) return false;
  for (size_t i = 0; i < length; ++i) {
    const char c = text[i];
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return false;
  }
  return true;
}

ParseResult parseCommand(const char* record, const size_t length, ParsedCommand& out) {
  out = ParsedCommand{};
  if (!record || length == 0 || length > RECORD_LIMIT) return ParseResult::MALFORMED;
  const char* cursor = record;
  const char* end = record + length;
  Field verb;
  Field id;
  if (!nextField(cursor, end, verb) || !nextField(cursor, end, id) || !validRequestId(id.text, id.length)) {
    return ParseResult::MALFORMED;
  }
  memcpy(out.requestId, id.text, REQUEST_ID_LENGTH);
  out.requestId[REQUEST_ID_LENGTH] = '\0';

  if (fieldIs(verb, "WIFI_JOIN")) {
    out.verb = Verb::WIFI_JOIN;
    if (cursor >= end) return ParseResult::BAD_FIELDS;
    out.chunk = cursor;
    out.chunkLength = static_cast<size_t>(end - cursor);
    return ParseResult::OK;
  }
  if (fieldIs(verb, "START_WIFI")) {
    out.verb = Verb::START_WIFI;
    return cursor == end ? ParseResult::OK : ParseResult::BAD_FIELDS;
  }

  if (fieldIs(verb, "PING") || fieldIs(verb, "START_AP") || fieldIs(verb, "CANCEL") || fieldIs(verb, "READ_LIST")) {
    out.verb = fieldIs(verb, "PING")       ? Verb::PING
               : fieldIs(verb, "START_AP") ? Verb::START_AP
               : fieldIs(verb, "CANCEL")   ? Verb::CANCEL
                                           : Verb::READ_LIST;
    return cursor == end ? ParseResult::OK : ParseResult::BAD_FIELDS;
  }
  if (fieldIs(verb, "OFFER")) {
    out.verb = Verb::OFFER;
    Field total;
    Field crc;
    if (!nextField(cursor, end, total) || !nextField(cursor, end, crc) || cursor != end ||
        !parseDecimal(total, out.total) || !parseHex8(crc, out.crc)) {
      return ParseResult::BAD_FIELDS;
    }
    return ParseResult::OK;
  }
  if (fieldIs(verb, "W")) {
    out.verb = Verb::WRITE;
    Field seq;
    if (!nextField(cursor, end, seq, true) || !parseDecimal(seq, out.seq) || cursor >= end)
      return ParseResult::BAD_FIELDS;
    // The chunk is the rest of the record; it may contain spaces.
    out.chunk = cursor;
    out.chunkLength = static_cast<size_t>(end - cursor);
    return ParseResult::OK;
  }
  return ParseResult::UNKNOWN_VERB;
}

const char* badFieldsCode(const Verb verb) {
  switch (verb) {
    case Verb::OFFER:
      return "BAD_RECORD";
    case Verb::WRITE:
      return "BAD_CHUNK";
    default:
      return "BAD_COMMAND";
  }
}

bool allowedInWindow(const Verb verb) {
  return verb != Verb::START_AP && verb != Verb::START_WIFI && verb != Verb::WIFI_JOIN && verb != Verb::NONE;
}

size_t formatOk(const char* requestId, char* out, const size_t capacity) {
  if (!out || capacity == 0 || !usableId(requestId)) return finishRecord(0, out, capacity);
  return finishRecord(snprintf(out, capacity, "OK %s", requestId), out, capacity);
}

size_t formatError(const char* requestId, const char* code, char* out, const size_t capacity) {
  if (!out || capacity == 0) return 0;
  const char* id = usableId(requestId) ? requestId : "00000000";
  return finishRecord(snprintf(out, capacity, "ERR %s %s", id, code && code[0] ? code : "FAILED"), out, capacity);
}

size_t formatData(const char* requestId, const uint32_t seq, const char* chunk, const size_t chunkLength, char* out,
                  const size_t capacity) {
  if (!out || capacity == 0 || !usableId(requestId) || !chunk || chunkLength == 0 || chunkLength > MAX_DATA_CHUNK) {
    return finishRecord(0, out, capacity);
  }
  const int head = snprintf(out, capacity, "D %s %lu ", requestId, static_cast<unsigned long>(seq));
  if (head <= 0 || static_cast<size_t>(head) + chunkLength >= capacity ||
      static_cast<size_t>(head) + chunkLength > RECORD_LIMIT) {
    return finishRecord(0, out, capacity);
  }
  memcpy(out + head, chunk, chunkLength);
  out[head + chunkLength] = '\0';
  return static_cast<size_t>(head) + chunkLength;
}

size_t formatEnd(const char* requestId, const uint32_t totalBytes, const uint32_t crc, char* out,
                 const size_t capacity) {
  if (!out || capacity == 0 || !usableId(requestId)) return finishRecord(0, out, capacity);
  return finishRecord(snprintf(out, capacity, "END %s %lu %08lX", requestId, static_cast<unsigned long>(totalBytes),
                               static_cast<unsigned long>(crc)),
                      out, capacity);
}

size_t formatStatus(const char* model, const char* deviceId, const char* firmware, const bool inWindow, char* out,
                    const size_t capacity, const bool appWake) {
  if (!out || capacity == 0) return 0;
  return finishRecord(snprintf(out, capacity, "V=1;MODEL=%s;ID=%s;FW=%s;CAP=AP,HTTP,SD,COMMIT1,READ1%s;WIN=%d",
                               model ? model : "X4", deviceId ? deviceId : "", firmware ? firmware : "unknown",
                               appWake    ? ",WAKE1,WIFI1"
                               : inWindow ? ""
                                          : ",WIFI1",
                               inWindow ? 1 : 0),
                      out, capacity);
}

uint32_t crcUpdate(const uint32_t crc, const void* data, const size_t length) {
  if (!data || length == 0) return crc;
  return PocketDaily::Content::contentCrcUpdate(crc, static_cast<const uint8_t*>(data), length);
}

void ListChunker::begin(char* pieceBuffer, const size_t capacity) {
  piece = pieceBuffer;
  pieceCapacity = capacity;
  pieceLength = 0;
  pieceOffset = 0;
  seq = 0;
  total = 0;
  runningCrc = CRC_START;
  done = false;
}

size_t ListChunker::nextChunk(Source& source, char* chunk) {
  if (done || !piece || !chunk) return 0;
  size_t used = 0;
  while (used < MAX_DATA_CHUNK) {
    if (pieceOffset >= pieceLength) {
      pieceLength = source.nextPiece(piece, pieceCapacity);
      pieceOffset = 0;
      if (pieceLength == 0 || pieceLength > pieceCapacity) {
        pieceLength = 0;
        done = true;
        break;
      }
    }
    size_t take = pieceLength - pieceOffset;
    if (take > MAX_DATA_CHUNK - used) take = MAX_DATA_CHUNK - used;
    memcpy(chunk + used, piece + pieceOffset, take);
    pieceOffset += take;
    used += take;
  }
  if (used == 0) return 0;
  runningCrc = crcUpdate(runningCrc, chunk, used);
  total += static_cast<uint32_t>(used);
  ++seq;
  return used;
}

bool OfferAssembler::begin(const char* offerId, const uint32_t total, const uint32_t crc, char* out,
                           const size_t outCapacity) {
  reset();
  if (!usableId(offerId) || total == 0 || total > MAX_OFFER_TOTAL || !out || outCapacity < total + 1) return false;
  memcpy(id, offerId, sizeof(id));
  buffer = out;
  capacity = outCapacity;
  expectedTotal = total;
  expectedCrc = crc;
  return true;
}

OfferAssembler::Step OfferAssembler::write(const uint32_t seq, const char* chunk, const size_t chunkLength) {
  if (!buffer || !chunk || chunkLength == 0 || seq != nextSeq || chunkLength > expectedTotal - received) {
    return Step::BAD_CHUNK;
  }
  memcpy(buffer + received, chunk, chunkLength);
  received += chunkLength;
  runningCrc = crcUpdate(runningCrc, chunk, chunkLength);
  ++nextSeq;
  if (received < expectedTotal) return Step::NEED_MORE;
  buffer[received] = '\0';
  return crcFinish(runningCrc) == expectedCrc ? Step::COMPLETE : Step::BAD_CHUNK;
}

bool OfferAssembler::matches(const char* offerId) const {
  return buffer && offerId && strncmp(id, offerId, sizeof(id)) == 0;
}

void OfferAssembler::reset() {
  id[0] = '\0';
  buffer = nullptr;
  capacity = 0;
  expectedTotal = 0;
  expectedCrc = 0;
  runningCrc = CRC_START;
  nextSeq = 0;
  received = 0;
}

bool RecordQueue::push(const char* bytes, const size_t length, const uint32_t generation) {
  if (!bytes || length == 0 || length > RECORD_LIMIT) return false;
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(bytes[i]);
    if (c < 0x20 || c == 0x7F) return false;
  }
  const uint32_t writeAt = head.load(std::memory_order_relaxed);
  if (writeAt - tail.load(std::memory_order_acquire) >= DEPTH) {
    overflow.store(true, std::memory_order_release);
    return false;
  }
  const size_t slot = writeAt % DEPTH;
  memcpy(records[slot], bytes, length);
  lengths[slot] = static_cast<uint8_t>(length);
  generations[slot] = generation;
  head.store(writeAt + 1, std::memory_order_release);
  return true;
}

bool RecordQueue::pop(char* out, const size_t capacity, size_t& length, const uint32_t generation) {
  length = 0;
  if (!out || capacity <= RECORD_LIMIT) return false;
  for (;;) {
    const uint32_t readAt = tail.load(std::memory_order_relaxed);
    if (readAt == head.load(std::memory_order_acquire)) return false;
    // Copy out before releasing the slot so the producer cannot overwrite the
    // record while it is parsed.
    const size_t slot = readAt % DEPTH;
    const size_t size = lengths[slot];
    const bool current = generations[slot] == generation;
    memcpy(out, records[slot], size);
    out[size] = '\0';
    memset(records[slot], 0, sizeof(records[slot]));
    tail.store(readAt + 1, std::memory_order_release);
    if (current) {
      length = size;
      return true;
    }
  }
}

void RecordQueue::clear() {
  memset(records, 0, sizeof(records));
  head.store(0, std::memory_order_release);
  tail.store(0, std::memory_order_release);
  overflow.store(false, std::memory_order_release);
}

}  // namespace Pocket::NearbySync
