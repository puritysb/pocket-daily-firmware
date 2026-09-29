#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "NearbySyncService.h"
#include "ReadingSyncProtocol.h"

// READ_LIST / OFFER / W of Pocket Reading Sync over BLE v1
// (docs/reading-sync-ble-v1.md) on top of a running Nearby Sync Service.
// Everything runs on the owning loop: the service's callbacks only queue
// records. List and offer buffers exist only for the duration of an exchange
// (nothrow; ERR NO_MEMORY when they cannot be allocated), and at most one
// notification is sent per pump(), retried on later passes when the stack
// cannot queue it.
namespace Pocket::NearbySync {

class ReadingSyncSession {
 public:
  explicit ReadingSyncSession(Service& service);
  ~ReadingSyncSession();
  ReadingSyncSession(const ReadingSyncSession&) = delete;
  ReadingSyncSession& operator=(const ReadingSyncSession&) = delete;

  // READ_LIST, OFFER and W; returns false for any other verb.
  bool handle(const ParsedCommand& command);
  // One owner-loop pass: drops the exchange when the link changed, then sends
  // at most one queued reply or the next D/END record.
  void pump();
  // A list is streaming, an offer is being assembled, or a reply is queued.
  bool busy() const;
  // Lists delivered and offers stored since the session started (log only).
  uint16_t listsSent() const { return lists; }
  uint16_t offersStored() const { return offers; }
  void reset();

 private:
  struct ListWork;
  struct OfferWork;
  static constexpr size_t REPLY_SLOTS = 4;
  static constexpr size_t REPLY_BYTES = 48;
  // ~3 s of loop passes before a stuck notification abandons the exchange.
  static constexpr uint16_t MAX_SEND_RETRIES = 300;

  void followLink();
  void startList(const char* requestId);
  void startOffer(const ParsedCommand& command);
  void writeOffer(const ParsedCommand& command);
  void completeOffer();
  void abortOffer(const char* code);
  void queueReply(const char* record, size_t length);
  void queueError(const char* requestId, const char* code);
  bool sendPending();

  Service& service;
  uint32_t generation = 0;
  std::unique_ptr<ListWork> list;
  std::unique_ptr<OfferWork> offer;
  char abortedOfferId[REQUEST_ID_LENGTH + 1] = {};
  char replies[REPLY_SLOTS][REPLY_BYTES] = {};
  uint8_t replyHead = 0;
  uint8_t replyCount = 0;
  uint16_t retries = 0;
  uint16_t lists = 0;
  uint16_t offers = 0;
};

}  // namespace Pocket::NearbySync
