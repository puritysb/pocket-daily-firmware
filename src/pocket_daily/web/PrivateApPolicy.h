#pragma once

#include <cstddef>
#include <cstdint>

#include "pocket_daily/web/Profile.h"

namespace PocketDaily::Web {

// Policy for the Pocket Nearby Sync private-AP launch path (SEAM.md): heap
// gates measured on the X3, transfer-profile selection, watchdog arming for
// the bounded session, the power-loss-durable SD boot trail, and the
// BLE-delivered hotspot credentials. The activity keeps the state machine;
// these are the decisions it consults.

// One leased companion connection; the generic hotspot keeps its own four.
inline constexpr uint8_t kPrivateApMaxConnections = 1;

// The preflight runs before NimBLE allocates its controller, host task and
// GATT database.
bool nearbyStartAllowed(uint32_t freeHeap, uint32_t largestBlock);
// Guard the resulting steady state separately so a future library or
// configuration change fails back to Pocket Daily instead of reaching pairing
// with too little headroom and resetting the reader.
bool nearbyReadyAllowed(uint32_t freeHeap, uint32_t largestBlock);
// Heap gate for starting the transfer web server; the lightweight (Pocket /
// X3 File Transfer) profile has the smaller guard.
bool webStartAllowed(bool lightweightProfile, uint32_t freeHeap, uint32_t largestBlock);

Profile selectProfile(bool privateAp, bool deviceIsX3);

// Enrol the Arduino loop task on the task watchdog and widen it for the
// bounded private-AP session. Nearby Sync always exits through a chip
// restart, which restores the sdkconfig 5 s default. False = reconfigure
// failed (loop watchdog stays armed at its previous timeout).
bool armPrivateApWatchdog();

// Private-AP startup leaves a power-loss-durable trail on the SD card; each
// step is flushed so a hard hang still leaves it readable over File
// Transfer. Diagnostic only.
void apBootLog(const char* step, bool reset = false);

// Derive the hotspot credentials delivered over authenticated BLE: an SSID
// prefix from the pairing identity and a fresh random passkey.
void generatePrivateApCredentials(const char* deviceId, char* ssidOut, size_t ssidLen, char* passOut, size_t passLen);

}  // namespace PocketDaily::Web
