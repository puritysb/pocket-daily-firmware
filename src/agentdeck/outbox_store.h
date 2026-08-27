#pragma once
//
// outbox_store.h — M6 Outbox persistence.
//
// Decisions are recorded locally first and pushed when a connection exists, so
// being offline never blocks pressing a button. Records queue on the SD card
// and are drained by Feed::syncOnce() via POST /outbox; every acknowledged
// decision is deleted regardless of its per-decision status (expired/rejected
// are terminal — the daemon validated against live state and said no).
//
// M6 ships the store + push pipeline; nothing on this device *produces*
// records yet, because the only offline-pressable class (`day`) has no daemon
// producer until the M7 card modules. See docs/decision-card.md.
//
#include <cstdint>

namespace PocketDaily {
namespace OutboxStore {

// Product-owned durable queue. AgentDeck currently drains it, but its records
// and SD path belong to Pocket Daily so another Companion source can do so too.

// One recorded decision — field-for-field the OutboxDecision wire shape
// (AgentDeck shared/src/protocol.ts § Card Feed Pull Sync).
struct Record {
  char cardId[72];  // "session:" + prefixed session UUID
  char sessionId[64];
  char requestId[40];      // permission gate correlation ("" = none)
  char action[24];         // permission_decision|select_option|respond|send_prompt|dismiss|card_choice
  char choiceId[32];       // card_choice stable semantic id (never a button position)
  char decision[8];        // allow|deny ("" = n/a)
  int16_t index;           // select_option wire index; -1 = none
  char value[24];          // respond shortcut/value
  char question[160];      // prompt echo — daemon refuses when it no longer matches
  uint32_t recordedEpoch;  // best epoch at record time; 0 = no clock source
};

struct Queue {
  uint8_t count;
  static constexpr uint8_t CAP = 8;
  Record records[CAP];
};

// Append one record (drop-oldest when full) and persist. Returns false when
// the SD isn't ready — the decision is lost, which the caller must surface.
bool append(const Record& rec);

// Load the persisted queue. Empty queue (and false) on any format mismatch.
bool load(Queue& out);

// Overwrite the persisted queue (used after a push removed acknowledged
// records). An empty queue removes the file.
bool save(const Queue& q);

// Number of persisted records (0 when unreadable).
uint8_t pendingCount();

}  // namespace OutboxStore
}  // namespace PocketDaily

namespace AgentDeck {
namespace OutboxStore = PocketDaily::OutboxStore;
}
