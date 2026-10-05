#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "ReadingProgress.h"
#include "ReadingProgressStore.h"

struct RecentBook;

// The reader's half of a reading-progress exchange, shared by the HTTP routes
// (GET/POST /api/pocket/v1/reading) and Pocket Reading Sync over BLE
// (READ_LIST / OFFER). Runs on the owning loop only: SD reads, the digest, and
// the pending-offer write. Nothing here computes an XPointer.
namespace PocketDaily::ReadingProgress {

// Per-operation scratch (~2.2 KB): allocate with makeUniqueNoThrow, never on a
// task stack.
struct ExchangeWork {
  Record record;
  Scratch scratch = {};
  char entry[MAX_XPOINTER_BYTES + 512] = {};  // one serialized list piece
};

struct ExchangeBook {
  std::string cachePath;
  bool hasRecord = false;
  bool current = false;  // the record describes the saved page
  float percentage = 0.0f;
  char document[DIGEST_HEX + 1] = {};
  char filenameDocument[DIGEST_HEX + 1] = {};
};

// Loads what the reader recorded for one recent EPUB and its digests. The
// partial-MD5 digest comes from the record when the file size still matches;
// otherwise it is recomputed (12 reads of 1 KB) without writing anything.
bool loadExchangeBook(const RecentBook& recent, ExchangeWork& work, ExchangeBook& book);

// The reading-list body, one piece per call (head, each listed book, tail), in
// Recent Books order within the list budget. With `includePaths` false the
// `path` members are omitted (BLE). Stateful; one stream per exchange.
class ListStream {
 public:
  ListStream(const char* deviceId, bool includePaths, ExchangeWork& work);
  // Writes the next piece into `out` and returns its length; 0 at the end.
  size_t next(char* out, size_t capacity);
  size_t listed() const { return composer.listed(); }
  size_t total() const { return composer.total(); }

 private:
  enum class Phase : uint8_t { HEAD, BOOKS, DONE };
  char deviceId[9] = {};
  bool includePaths;
  ExchangeWork& work;
  ListComposer composer;
  Phase phase = Phase::HEAD;
  size_t index = 0;
};

enum class OfferStoreResult : uint8_t { STORED, UNKNOWN_DOCUMENT, STALE_POSITION, FAILED };

// Stores a validated offer as the pending place of the recent book whose
// `document` or `filenameDocument` matches. The current position is never
// touched; the reader asks when the book next opens.
OfferStoreResult storeOffer(const OfferRequest& request, ExchangeWork& work);

}  // namespace PocketDaily::ReadingProgress
