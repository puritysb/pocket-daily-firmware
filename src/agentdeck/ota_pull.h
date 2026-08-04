#pragma once
//
// ota_pull.h — feed-carried firmware update (contract § Pull OTA).
//
// A battery device on the wake-sync-sleep cadence never holds a WS socket, so
// the daemon's WS OTA push can't reach it. Instead the daemon stages a build
// (`agentdeck esp32-ota <board> --firmware … --stage`) and every `GET /feed`
// response carries a tiny `fw: {size, md5}` advert for that board. On the next
// pull the device downloads `GET /esp32/fw`, validates, and flashes itself —
// updates ride the existing 15-60 min cadence with no user interaction.
//
#include <cstdint>

namespace AgentDeck {
namespace OtaPull {

// Act on a feed's fw advert: guards (battery / WS transfer active / already
// applied / slot size), then blocking download (~30-60 s LAN) to the shared
// OTA cache, MD5 + structural validation, and flashPending handoff — the
// dashboard loop paints the notice and OtaWs::serviceFlash restarts.
// Call from the loop task with a shallow stack (the pull path), never from a
// WS callback. True = image staged, flash imminent: the caller must NOT enter
// timed sleep (the existing flashPending guards already ensure this).
bool tryInstall(const char* ip, uint16_t port, const char* token, const char* board, uint32_t fwSize, const char* fwMd5,
                int battPct);

}  // namespace OtaPull
}  // namespace AgentDeck
