#pragma once
//
// deck_store.h — M5.5 Deck persistence.
//
// Persists the Face's deck (the bounded set of session cards) to the SD card so
// the device can render the last-synced deck at boot — before Wi-Fi joins or the
// daemon is found — with an honest "as of" sync timestamp. E-ink is a surface
// that always shows something; this store is what it shows when nothing is live.
//
// Deliberately display-only: a cached card carries no requestId/options and can
// never become actionable. Decisions require a live daemon (the Outbox is M6).
//
#include <cstdint>

#include "agent_state.h"
#include "agentdeck_config.h"
#include "glance_state.h"

namespace AgentDeck {
namespace DeckStore {

// One persisted overview card — the render-facing subset of SessionInfo.
struct Record {
  char sid[64];
  char project[40];
  char agentType[16];
  char state[20];
  char activity[SESSION_ACTIVITY_CAP];
  uint8_t awaiting;
  // M6 card validity class ("live"/"day"/"info", card_class.h). A cached
  // `live` card is rendered greyed with a "reconnect to act" hint — it must
  // never look answerable offline. Changing Record's shape bumps recordSize,
  // which invalidates pre-M6 cache files by design.
  char actionClass[6];
};

struct Snapshot {
  uint32_t savedEpoch;  // unix seconds at save; 0 = no clock source was available
  uint8_t count;
  // Card-feed content signature at save time — echoed as `GET /feed?sig=` on
  // the next pull so an unchanged night costs one tiny response (v2). Empty
  // when the deck was cached from the WS live mode (no sig on that path).
  char deckSig[12];
  // Sleep-glance block (weather / usage / wrap-up) as of the save, so the
  // offline/unchanged sleep frame can render it without a fresh feed (v2).
  GlanceInfo glance;
  // Daemon-authored Pocket cards are device-consumable day/info content, so
  // they survive reboot beside the session deck instead of vanishing offline.
  uint8_t pocketCount;
  PocketCard pocketCards[POCKET_CARD_CAP];
  Record records[AgentDeckCfg::SESSIONS_CAP];
};

// Write the snapshot to the SD card (tmp + rename so a power cut mid-write
// leaves the previous deck intact). Returns false when the SD isn't ready or
// the write fails; the caller just retries on the next deck change.
bool save(const Snapshot& snap);

// Load the persisted deck. On any failure (missing file, format/version
// mismatch, short read) `out` is left as an empty snapshot and this returns
// false — the Face then renders its normal empty state.
bool load(Snapshot& out);

}  // namespace DeckStore
}  // namespace AgentDeck
