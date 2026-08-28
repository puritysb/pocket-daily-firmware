#pragma once
//
// protocol.h — TRIMMED port of AgentDeck esp32/src/net/protocol.{h,cpp}.
//
// Parses inbound bridge JSON and updates g_state. M2 handles the message types
// a display-only client needs: state_update, sessions_list, usage_update, and
// the connection/connected acknowledgements. Other message types are accepted
// and ignored (see protocol.cpp for the M3-stubbed list).
//
// NOTE: esp32_ota_* frames never reach this parser — ws_client.cpp routes them
// to OtaWs::maybeHandleFrame() first, because this parser's ArduinoJson filter
// would silently drop the base64 chunk payload (`data`) and kill the transfer.
//
#include <cstddef>
#include <cstdint>

#include "pocket_daily/font_pack_sync.h"
#include "pocket_daily/learning_pack_sync.h"

namespace AgentDeck {
namespace Protocol {

// Parse one inbound JSON frame and apply it to g_state (thread-safe via mutex).
void parseMessage(const char* json, size_t length);

// Result of applying a `GET /feed` body. `unchanged` = the daemon answered the
// conditional pull's sig echo with "keep your cache": sessions/glance were NOT
// touched (and dataReceived stays false, so the render keeps falling back to
// the persisted deck), but serverTime/serverHm still re-anchored the clock.
struct FeedApply {
  bool ok = false;
  bool unchanged = false;
  uint32_t nextPullSec = 0;
  // Content signature echoed on the next pull (persisted with the deck cache).
  char deckSig[12] = {0};
  // Staged firmware advertised by the daemon (contract § Pull OTA). Rides both
  // full and `unchanged` responses; size 0 = nothing staged for this board.
  uint32_t fwSize = 0;
  char fwMd5[36] = {0};
  char fwProductId[40] = {0};
  char fwBoard[20] = {0};
  char fwUpdateChannel[16] = {0};
  // Optional SD learning-pack advert. It is parsed even on an unchanged feed,
  // just like firmware, so explicit Sync can update course content without
  // forcing the provider to mutate the user's card deck.
  PocketDaily::LearningPackSync::Advert learningPack;
  // Optional broad SD font pack advert, available on full and unchanged feeds.
  PocketDaily::FontPackSync::Advert fontPack;
};

// M6 pull sync: parse a `GET /feed` card_feed body and land the embedded
// sessions in g_state exactly like a sessions_list frame would, plus re-anchor
// the daemon clock estimate from `serverTime` (the drifty-RTC "as of" source)
// and daemon-local wall time from `serverHm`, and land the sleep-glance block
// (weather / usage / wrap-up) in g_state.glance.
// Contract: AgentDeck shared/src/protocol.ts § Card Feed Pull Sync + § Glance.
FeedApply applyCardFeed(const char* json, size_t length);

}  // namespace Protocol
}  // namespace AgentDeck
