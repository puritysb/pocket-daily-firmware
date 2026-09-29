#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

// Pocket Reading Sync over BLE v1 (docs/reading-sync-ble-v1.md): the record
// grammar carried by the Nearby Sync command/event characteristics. Pure and
// allocation-free so the host tests exercise exactly what the reader runs; the
// radio and SD owners live in NearbySyncService and ReadingSyncSession.
namespace Pocket::NearbySync {

// v1 record limit shared with NearbySyncService::MAX_RECORD_BYTES.
inline constexpr size_t RECORD_LIMIT = 220;
// Largest `D` chunk (reading list bytes per notification).
inline constexpr size_t MAX_DATA_CHUNK = 180;
// Largest `OFFER` body; the same limit as POST /api/pocket/v1/reading.
inline constexpr size_t MAX_OFFER_TOTAL = 1024;
inline constexpr size_t REQUEST_ID_LENGTH = 8;

enum class Verb : uint8_t { NONE, PING, START_AP, CANCEL, READ_LIST, OFFER, WRITE };

enum class ParseResult : uint8_t {
  OK,
  MALFORMED,     // no usable request id: reply ERR 00000000 BAD_COMMAND
  UNKNOWN_VERB,  // valid id, unknown verb: ERR <id> UNKNOWN_COMMAND
  BAD_FIELDS,    // known verb and id, bad arguments: ERR <id> BAD_RECORD / BAD_CHUNK / BAD_COMMAND
};

struct ParsedCommand {
  Verb verb = Verb::NONE;
  char requestId[REQUEST_ID_LENGTH + 1] = {};
  uint32_t seq = 0;    // W
  uint32_t total = 0;  // OFFER
  uint32_t crc = 0;    // OFFER
  // W: the chunk, pointing into the parsed record (valid while it lives).
  const char* chunk = nullptr;
  size_t chunkLength = 0;
};

// Fields are separated by exactly one space; the W chunk is the rest of the
// record and may contain spaces. `record` need not be NUL-terminated.
ParseResult parseCommand(const char* record, size_t length, ParsedCommand& out);
bool validRequestId(const char* text, size_t length);

// The error code for a BAD_FIELDS parse of `verb`.
const char* badFieldsCode(Verb verb);
// Commands an exchange window serves. START_AP needs the Nearby Sync screen
// (physical presence) and is refused with NOT_IN_SYNC.
bool allowedInWindow(Verb verb);
inline constexpr char NOT_IN_SYNC[] = "NOT_IN_SYNC";

// Event records. Each returns the record length, or 0 when it would exceed
// RECORD_LIMIT or `capacity` (always NUL-terminated when capacity > 0).
size_t formatOk(const char* requestId, char* out, size_t capacity);
size_t formatError(const char* requestId, const char* code, char* out, size_t capacity);
size_t formatData(const char* requestId, uint32_t seq, const char* chunk, size_t chunkLength, char* out,
                  size_t capacity);
// CRC as eight upper-case hex digits (parsers accept either case).
size_t formatEnd(const char* requestId, uint32_t totalBytes, uint32_t crc, char* out, size_t capacity);
// Status characteristic value: v1 status plus READ1 and WIN=1|0.
size_t formatStatus(const char* model, const char* deviceId, const char* firmware, bool inWindow, char* out,
                    size_t capacity);

// CRC-32 (IEEE 802.3, reflected 0xEDB88320; zlib crc32 and Swift's matching
// implementation). Start with CRC_START, finish with crcFinish.
inline constexpr uint32_t CRC_START = 0xFFFFFFFFu;
uint32_t crcUpdate(uint32_t crc, const void* data, size_t length);
inline uint32_t crcFinish(const uint32_t crc) { return crc ^ 0xFFFFFFFFu; }

// Frames a body produced piece by piece (the reading list: head, entries,
// tail) into D chunks of at most MAX_DATA_CHUNK bytes, and keeps the running
// length and CRC for END. The piece buffer is caller-owned and reused.
class ListChunker {
 public:
  // Returns the next piece in `out` (at most `capacity` bytes) or 0 at the end
  // of the body. Called only from the owning loop.
  class Source {
   public:
    virtual size_t nextPiece(char* out, size_t capacity) = 0;

   protected:
    ~Source() = default;
  };

  void begin(char* pieceBuffer, size_t pieceCapacity);
  // Fills `chunk` (capacity >= MAX_DATA_CHUNK) with the next bytes; returns the
  // chunk length, or 0 once the whole body was framed.
  size_t nextChunk(Source& source, char* chunk);
  uint32_t nextSeq() const { return seq; }
  uint32_t totalBytes() const { return total; }
  uint32_t crc() const { return crcFinish(runningCrc); }
  bool finished() const { return done; }

 private:
  char* piece = nullptr;
  size_t pieceCapacity = 0;
  size_t pieceLength = 0;
  size_t pieceOffset = 0;
  uint32_t seq = 0;
  uint32_t total = 0;
  uint32_t runningCrc = CRC_START;
  bool done = false;
};

// Reassembles one OFFER body from W chunks into a caller-owned buffer.
class OfferAssembler {
 public:
  enum class Step : uint8_t { NEED_MORE, COMPLETE, BAD_CHUNK };

  // False (BAD_RECORD) for a zero or oversized total, or a buffer too small.
  bool begin(const char* offerId, uint32_t total, uint32_t crc, char* out, size_t outCapacity);
  // Chunks must arrive in order (seq 0,1,2,...) and not overrun the total; the
  // body completes when all bytes arrived and the CRC matches. A bad chunk
  // ends the offer.
  Step write(uint32_t seq, const char* chunk, size_t chunkLength);
  bool active() const { return buffer != nullptr; }
  bool matches(const char* offerId) const;
  const char* requestId() const { return id; }
  const char* body() const { return buffer; }
  size_t length() const { return received; }
  void reset();

 private:
  char id[REQUEST_ID_LENGTH + 1] = {};
  char* buffer = nullptr;
  size_t capacity = 0;
  uint32_t expectedTotal = 0;
  uint32_t expectedCrc = 0;
  uint32_t runningCrc = CRC_START;
  uint32_t nextSeq = 0;
  size_t received = 0;
};

// Command records between the NimBLE host task (single producer, in the write
// callback) and the owning loop (single consumer). Each record is tagged with
// the connection generation it arrived on; pop() drops records from an older
// connection. Fixed storage, no allocation, no locks.
class RecordQueue {
 public:
  static constexpr size_t DEPTH = 4;

  // Producer. Rejects control characters (bytes >= 0x80 are allowed: a W chunk
  // carries raw UTF-8 of the offer body, possibly split mid-character). A full
  // queue drops the record and flags an overflow.
  bool push(const char* bytes, size_t length, uint32_t generation);
  // Consumer. Copies the oldest record of `generation` into `out` (capacity >
  // RECORD_LIMIT, NUL-terminated); false when none is queued.
  bool pop(char* out, size_t capacity, size_t& length, uint32_t generation);
  // True once after push() dropped a record because the queue was full.
  bool takeOverflow() { return overflow.exchange(false, std::memory_order_acq_rel); }
  // Only while no producer can run (before NimBLE starts or after it stopped).
  void clear();

 private:
  char records[DEPTH][RECORD_LIMIT] = {};
  uint8_t lengths[DEPTH] = {};
  uint32_t generations[DEPTH] = {};
  std::atomic<uint32_t> head{0};  // next slot the producer writes
  std::atomic<uint32_t> tail{0};  // next slot the consumer reads
  std::atomic<bool> overflow{false};
};
static_assert(RECORD_LIMIT <= 255, "queued record lengths are stored in one byte");

}  // namespace Pocket::NearbySync
