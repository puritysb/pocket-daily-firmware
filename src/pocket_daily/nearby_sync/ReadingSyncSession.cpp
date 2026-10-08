#include "ReadingSyncSession.h"

#include <Arduino.h>
#include <HalSystem.h>
#include <Logging.h>
#include <Memory.h>

#include <cstring>

#include "pocket_daily/ReadingExchange.h"

namespace Pocket::NearbySync {
namespace Reading = PocketDaily::ReadingProgress;

namespace {
// BLE also needs radio headroom while the scratch, JSON parser and SD handles
// are live. These gates are deliberately stricter than the HTTP list route.
constexpr uint32_t EXCHANGE_MIN_FREE = 24U * 1024U;
constexpr uint32_t EXCHANGE_MIN_BLOCK = 8U * 1024U;

bool exchangeHeapAvailable() {
  return ESP.getFreeHeap() >= EXCHANGE_MIN_FREE && ESP.getMaxAllocHeap() >= EXCHANGE_MIN_BLOCK;
}

// Adapts the shared list stream to the chunker.
class StreamSource final : public ListChunker::Source {
 public:
  explicit StreamSource(Reading::ListStream& stream) : stream(stream) {}
  size_t nextPiece(char* out, const size_t capacity) override { return stream.next(out, capacity); }

 private:
  Reading::ListStream& stream;
};
}  // namespace

// ~2.4 KB for one READ_LIST: the shared exchange scratch (whose entry buffer is
// the chunker's piece buffer), the stream state and the pending record.
struct ReadingSyncSession::ListWork {
  explicit ListWork(const char* deviceId) : stream(deviceId, false, work) {}
  Reading::ExchangeWork work;
  Reading::ListStream stream;
  ListChunker chunker;
  char id[REQUEST_ID_LENGTH + 1] = {};
  char chunk[MAX_DATA_CHUNK] = {};
  char record[MAX_RECORD_BYTES + 1] = {};
  size_t recordLength = 0;  // 0: nothing waiting to be sent
  bool endQueued = false;   // `record` holds END
};

// One OFFER body (<= 1 KiB) while its W chunks arrive.
struct ReadingSyncSession::OfferWork {
  OfferAssembler assembler;
  char body[MAX_OFFER_TOTAL + 1] = {};
};

ReadingSyncSession::ReadingSyncSession(Service& service)
    : service(service), generation(service.connectionGeneration()) {}

ReadingSyncSession::~ReadingSyncSession() { reset(); }

void ReadingSyncSession::reset() {
  list.reset();
  offer.reset();
  abortedOfferId[0] = '\0';
  replyHead = 0;
  replyCount = 0;
  retries = 0;
}

bool ReadingSyncSession::busy() const { return list || offer || replyCount; }

void ReadingSyncSession::followLink() {
  const uint32_t current = service.connectionGeneration();
  if (current == generation) return;
  // A disconnect (or a new peer) discards incomplete exchanges and replies.
  if (busy()) LOG_DBG("RSYNC", "Link changed; exchange discarded");
  reset();
  generation = current;
}

bool ReadingSyncSession::handle(const ParsedCommand& command) {
  // Commands come only from the current link (the record queue drops older
  // ones); anything left from an earlier link goes first.
  followLink();
  switch (command.verb) {
    case Verb::READ_LIST:
      startList(command.requestId);
      return true;
    case Verb::OFFER:
      startOffer(command);
      return true;
    case Verb::WRITE:
      writeOffer(command);
      return true;
    default:
      return false;
  }
}

void ReadingSyncSession::startList(const char* requestId) {
  if (list || offer) {
    queueError(requestId, "BUSY");
    return;
  }
  if (!exchangeHeapAvailable()) {
    queueError(requestId, "NO_MEMORY");
    return;
  }
  list = makeUniqueNoThrow<ListWork>(service.deviceId());
  if (!list) {
    queueError(requestId, "NO_MEMORY");
    return;
  }
  memcpy(list->id, requestId, sizeof(list->id));
  list->chunker.begin(list->work.entry, sizeof(list->work.entry));
  retries = 0;
  HalSystem::setCrashBreadcrumb("readsync:exchange-list");
}

void ReadingSyncSession::startOffer(const ParsedCommand& command) {
  // A new OFFER discards an incomplete one.
  if (offer) {
    LOG_DBG("RSYNC", "Offer %s replaced before completion", offer->assembler.requestId());
    offer.reset();
  }
  if (list) {
    queueError(command.requestId, "BUSY");
    return;
  }
  if (command.total == 0 || command.total > MAX_OFFER_TOTAL) {
    queueError(command.requestId, "BAD_RECORD");
    return;
  }
  if (!exchangeHeapAvailable()) {
    queueError(command.requestId, "NO_MEMORY");
    return;
  }
  offer = makeUniqueNoThrow<OfferWork>();
  if (!offer) {
    queueError(command.requestId, "NO_MEMORY");
    return;
  }
  if (!offer->assembler.begin(command.requestId, command.total, command.crc, offer->body, sizeof(offer->body))) {
    offer.reset();
    queueError(command.requestId, "BAD_RECORD");
    return;
  }
  abortedOfferId[0] = '\0';
  HalSystem::setCrashBreadcrumb("readsync:exchange-offer");
}

void ReadingSyncSession::writeOffer(const ParsedCommand& command) {
  if (!offer || !offer->assembler.matches(command.requestId)) {
    // Chunks still in flight for an offer that already failed were answered
    // by its single ERR.
    if (strcmp(abortedOfferId, command.requestId) != 0) queueError(command.requestId, "BAD_CHUNK");
    return;
  }
  switch (offer->assembler.write(command.seq, command.chunk, command.chunkLength)) {
    case OfferAssembler::Step::NEED_MORE:
      return;
    case OfferAssembler::Step::BAD_CHUNK:
      abortOffer("BAD_CHUNK");
      return;
    case OfferAssembler::Step::COMPLETE:
      completeOffer();
      return;
  }
}

void ReadingSyncSession::completeOffer() {
  // Heap can shrink while W chunks arrive. Recheck before JSON and SD work.
  if (!exchangeHeapAvailable()) {
    abortOffer("NO_MEMORY");
    return;
  }
  struct StoreWork {
    Reading::OfferRequest request;
    Reading::ExchangeWork work;
  };
  char id[REQUEST_ID_LENGTH + 1];
  memcpy(id, offer->assembler.requestId(), sizeof(id));

  auto store = makeUniqueNoThrow<StoreWork>();
  if (!store) {
    abortOffer("NO_MEMORY");
    return;
  }
  // The same validation as POST /api/pocket/v1/reading.
  const char* error = nullptr;
  bool outOfMemory = false;
  if (!Reading::parseOfferJson(offer->assembler.body(), offer->assembler.length(), store->request, error,
                               outOfMemory)) {
    LOG_ERR("RSYNC", "Offer %s rejected: %s", id, error ? error : "invalid");
    abortOffer(outOfMemory ? "NO_MEMORY" : "BAD_RECORD");
    return;
  }
  // The body names the reader it is meant for, as over HTTP.
  if (strcmp(store->request.deviceID, service.deviceId()) != 0) {
    LOG_ERR("RSYNC", "Offer %s is for another reader", id);
    abortOffer("BAD_RECORD");
    return;
  }
  offer.reset();  // the body is parsed; release it before the SD pass
  switch (Reading::storeOffer(store->request, store->work)) {
    case Reading::OfferStoreResult::STORED: {
      char record[REPLY_BYTES];
      const size_t length = formatOk(id, record, sizeof(record));
      if (length) queueReply(record, length);
      offers++;
      HalSystem::setCrashBreadcrumb("readsync:exchange-offer-stored");
      LOG_INF("RSYNC", "Offer %s stored: %.4f from %s", id, static_cast<double>(store->request.offer.percentage),
              store->request.offer.device);
      return;
    }
    case Reading::OfferStoreResult::UNKNOWN_DOCUMENT:
      queueError(id, "UNKNOWN_DOCUMENT");
      return;
    case Reading::OfferStoreResult::STALE_POSITION:
      queueError(id, "STALE_POSITION");
      return;
    case Reading::OfferStoreResult::FAILED:
      queueError(id, "FAILED");
      return;
  }
}

void ReadingSyncSession::abortOffer(const char* code) {
  if (!offer) return;
  memcpy(abortedOfferId, offer->assembler.requestId(), sizeof(abortedOfferId));
  offer.reset();
  queueError(abortedOfferId, code);
}

void ReadingSyncSession::queueReply(const char* record, const size_t length) {
  if (!record || length == 0 || length >= REPLY_BYTES) return;
  if (replyCount >= REPLY_SLOTS) {
    LOG_ERR("RSYNC", "Reply dropped: %s", record);
    return;
  }
  char* slot = replies[(replyHead + replyCount) % REPLY_SLOTS];
  memcpy(slot, record, length);
  slot[length] = '\0';
  replyCount++;
}

void ReadingSyncSession::queueError(const char* requestId, const char* code) {
  char record[REPLY_BYTES];
  const size_t length = formatError(requestId, code, record, sizeof(record));
  if (length) queueReply(record, length);
}

void ReadingSyncSession::pump() {
  followLink();
  if (service.takeOverflow() && offer) {
    // A W chunk was dropped: the offer cannot complete; the app may retry.
    abortOffer("BUSY");
  }
  if (!sendPending() && list && list->recordLength == 0 && !list->endQueued) {
    // Frame the next D record (or END) now; it is sent on the next pass.
    StreamSource source(list->stream);
    const size_t length = list->chunker.nextChunk(source, list->chunk);
    if (length) {
      list->recordLength =
          formatData(list->id, list->chunker.nextSeq() - 1, list->chunk, length, list->record, sizeof(list->record));
    } else {
      list->recordLength =
          formatEnd(list->id, list->chunker.totalBytes(), list->chunker.crc(), list->record, sizeof(list->record));
      list->endQueued = true;
    }
    if (list->recordLength == 0) {
      LOG_ERR("RSYNC", "List %s could not be framed", list->id);
      char id[REQUEST_ID_LENGTH + 1];
      memcpy(id, list->id, sizeof(id));
      list.reset();
      queueError(id, "FAILED");
    }
  }
}

// Sends at most one record: a queued reply first, then the list's next
// record. True when a send was attempted this pass.
bool ReadingSyncSession::sendPending() {
  const char* record = nullptr;
  if (replyCount) {
    record = replies[replyHead];
  } else if (list && list->recordLength) {
    record = list->record;
  } else {
    return false;
  }
  if (!service.notifyRecord(record)) {
    if (++retries > MAX_SEND_RETRIES) {
      LOG_ERR("RSYNC", "Notification not accepted; exchange abandoned");
      reset();
    }
    return true;
  }
  retries = 0;
  if (replyCount) {
    replyHead = static_cast<uint8_t>((replyHead + 1) % REPLY_SLOTS);
    replyCount--;
    return true;
  }
  list->recordLength = 0;
  if (list->endQueued) {
    lists++;
    HalSystem::setCrashBreadcrumb("readsync:exchange-list-sent");
    LOG_INF("RSYNC", "List %s sent: %u books, %lu bytes", list->id, static_cast<unsigned>(list->stream.listed()),
            static_cast<unsigned long>(list->chunker.totalBytes()));
    list.reset();
  }
  return true;
}

}  // namespace Pocket::NearbySync
