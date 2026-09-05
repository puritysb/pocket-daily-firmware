# Pocket Nearby Sync v1

Pocket Nearby Sync is the account-free link between the Pocket app and a
Xteink X3 or X4 running Crosspoint Pocket firmware. It is independent of AgentDeck and does not require
the phone and reader to be on the same infrastructure Wi-Fi network.

## Product contract

- The SD card is the source of truth for device content.
- The Apple app keeps a local mirror and can operate without a Pocket server.
- Bluetooth LE is a bounded control plane, not the bulk file transport.
- Pocket's verified HTTP commit API is the normal bulk path. Existing
  CrossPoint WebSocket, WebDAV, and direct SD-card formats remain compatible
  recovery paths.
- AgentDeck Dashboard is an optional provider and is not involved in pairing,
  content, learning state, or firmware updates.
- Deep sleep turns both radios off. Nearby Sync therefore starts only after the
  person wakes the device, opens Pocket Daily, and presses its `Sync` action.
- Nearby Sync belongs to Pocket Daily and is not shown in CrossPoint's generic
  File Transfer mode selector. File Transfer remains the manual recovery path.

## Transport selection

The transport design has one reliable path and two optional shortcuts:

1. BLE performs discovery, authenticated status, and requests a temporary
   device hotspot.
2. The temporary hotspot carries files, fonts, content packs, screenshots, and
   firmware.
3. Existing LAN discovery through `_crosspoint._tcp` is an opportunistic fast
   path only; failure never prevents BLE/private-hotspot sync.
4. User-selected SD-card access on macOS is the offline and bootstrap path.

BLE and Wi-Fi are never kept active together on the no-PSRAM ESP32-C3. The
device sends the hotspot lease over the encrypted BLE connection, stops and
deinitializes BLE, and only then starts Wi-Fi AP mode.

## BLE service

Pocket advertises only after the Pocket Daily `Sync` action opens its dedicated
Nearby Sync screen.

| Item | Value |
| --- | --- |
| Local name | `Pocket-XXXX` (`XXXX` is a non-secret device-id suffix) |
| Service | `7b8d5001-8e5b-4a7e-9d9a-7e42d2c50001` |
| Status characteristic | `7b8d5002-8e5b-4a7e-9d9a-7e42d2c50001` |
| Command characteristic | `7b8d5003-8e5b-4a7e-9d9a-7e42d2c50001` |
| Event characteristic | `7b8d5004-8e5b-4a7e-9d9a-7e42d2c50001` |

All characteristic values are UTF-8 and at most 220 bytes in v1. Commands and
events are newline-free ASCII records so neither side needs a JSON allocator
on the BLE path.

### Status

Status is readable only on an encrypted, authenticated connection:

```text
V=1;MODEL=X3;ID=89ABCDEF;FW=1.4.1;CAP=AP,HTTP,SD,COMMIT1
```

`MODEL` is `X3` or `X4`; the app uses it to select the 528×792 X3 or 480×800
X4 surface and matching physical-control geometry. Unknown fields must be
ignored. `V`, `MODEL`, `ID`, and `CAP` are required.

### Commands

The command characteristic requires an encrypted, authenticated write.

```text
PING <request-id>
START_AP <request-id>
CANCEL <request-id>
```

`request-id` is eight uppercase hexadecimal digits chosen by the app. The
device accepts one command at a time and never performs Wi-Fi or SD work in a
BLE callback; callbacks only copy the bounded record into the activity queue.

### Events

The encrypted event characteristic supports notifications:

```text
OK <request-id>
ERR <request-id> <code>
AP <request-id> <ssid> <passphrase> <ipv4> <http-port> <legacy-ws-port> <lease-seconds>
```

Fields never contain spaces. The v1 hotspot passphrase is 12 uppercase
hexadecimal characters. The app must treat it as ephemeral and must not sync it
to cloud storage or diagnostics. `legacy-ws-port` is `0` for the verified HTTP
Pocket path; the field remains in v1 so older parsers retain record alignment.

The device sends `AP`, waits long enough for the notification to be delivered,
disconnects BLE, releases the Bluetooth controller memory, and starts the AP.
The app then joins the SSID with the platform hotspot configuration API and
verifies `GET /api/status` before transferring any data.

On iOS/iPadOS the app uses `NEHotspotConfiguration` with `joinOnce`. On macOS,
where that API is unavailable, Pocket uses the public CoreWLAN client API to
scan for the advertised SSID and associate with the one-time passphrase. Modern
macOS gates Wi-Fi scanning behind Location Services, so the first sync asks for
location permission once; Pocket does not request coordinates or persist SSID,
BSSID, or passphrase data. Manual SSID/password entry remains visible only when
automatic association fails.

## Pairing and authorization

- BLE Secure Connections, MITM protection, and bonding are required.
- Pocket acts as display-only and shows a random six-digit passkey.
- The app asks the person to enter the displayed passkey.
- Pressing `Sync` in Pocket Daily is the physical-presence authorization window. The
  device advertises for at most two minutes without a connection.
- A bonded client still cannot start a hotspot unless the Nearby Sync screen is
  currently open.
- `START_AP` is acknowledged only after the device has generated a new SSID,
  passphrase, and short lease.

## Hotspot and bulk transfer

Nearby Sync uses a versioned, interruption-safe bulk API:

- `GET /api/status` on port 80 verifies identity and mode.
- `/api/status` advertises `uploadStreamPort` (currently TCP port 82). The app
  opens that port once, sends `POCKET-PUT/1`, staging path, and byte length,
  then streams the file on the same connection. The reader returns the received
  length and CRC32 before the app is allowed to commit.
- `POST /api/pocket/v1/commit` supplies the staging path, final path, byte
  length, and CRC32. The reader checks all four against the completed upload,
  then publishes it. A disconnect cannot overwrite the previous final file.
- The private Pocket AP uses a minimal server profile: status, a compact
  `/api/pocket/v1/preferences` resource for the companion's four settings,
  upload, and verified commit only. It never constructs the full localized
  `/api/settings` registry at the X3's lowest-heap point and does not start captive DNS, mDNS,
  discovery UDP, WebDAV, or the legacy WebSocket listener. The generic File
  Transfer screen retains the complete browser/WebSocket/WebDAV profile.
- HTTP and font upload buffers are allocated only for the active transfer and
  released at completion, instead of permanently consuming 8 KiB of X3 heap.
- File uploads use a 1 KiB second-stage SD buffer. The HTTP layer already
  supplies roughly 1.4 KiB multipart pieces, while the previous 4 KiB
  allocation could fail on a fragmented no-PSRAM X3 before the first byte was
  accepted. If even the 1 KiB optimization cannot be allocated, the server
  writes those framework-owned pieces directly to SD instead of rejecting the
  transfer.
- The stream listener services one bounded slice per activity-loop pass, then
  returns to the global input loop, so a firmware upload uses one TCP socket
  without blocking physical buttons. Socket bytes are batched into a transient
  4 KiB buffer (allocated only for the active transfer; a 768-byte static
  buffer is the allocation-free fallback) so every SD flush is one
  sector-aligned multi-block write instead of a sub-sector read-modify-write,
  and the loop watchdog is suspended once per flush instead of once per
  768-byte read. The staging file is preallocated to its final size when the
  card can provide a contiguous span, which removes mid-transfer cluster
  allocation stalls and leaves `update.bin` contiguous for the flasher.
- The request header accepts an optional `Resume: 1` line. A disconnect or
  30-second idle timeout during payload keeps the hidden staging file and the
  verified prefix (path, size, flushed byte count, running CRC) in RAM for the
  rest of the AP session; protocol and SD failures still delete it. A resume
  request for the same path and size is answered with `RESUME <received>`
  before any payload is read, and the companion sends only the remainder while
  rebuilding the CRC over the retained prefix. `/api/status` advertises
  `uploadStreamResume` so older readers keep receiving the v1 header. Stale
  `.pocket-*.part` leftovers in the destination directory are removed when a
  new transfer begins; `.pocket-backup.part` is never touched. The previous
  published book or firmware remains intact in every case. Older firmware
  still falls back to its advertised HTTP upload capability.
- A `.bin` is always published as `/update.bin`; transport never flashes it.
  Before publication, the Apple companion requires an ESP32-C3 application
  image with valid segment bounds, XOR checksum, optional appended SHA-256, and
  both the `CrossPoint version:` and `PocketNearbySync` product markers. Release
  builds must retain those stable markers. The existing on-device validator and
  explicit user confirmation remain the installation boundary.

The product hotspot is WPA2 protected and permits one client. Its advertised
lease is an idle lease: each recognized companion HTTP request and each
upload-stream progress slice restarts the 300-second countdown. Pocket checks
status every 15 seconds while connected, so an open companion session remains
available; three consecutive failures clear the app's connected state. The AP
shuts down after 300 seconds without companion activity or immediately when the
person leaves the transfer screen. Manual `Create Hotspot` remains available
for compatibility, but the app-assisted path uses per-session credentials.

## Crash diagnostics

A panic, CPU lockup, watchdog, brownout, eFuse, or power-glitch reboot writes
`/crash_report.txt` automatically and rotates the three previous reports as
`crash_report.1.txt` through `crash_report.3.txt`. The latest report includes
the firmware version, hardware reset reason, panic reason when one exists,
retained log ring, stack memory, and an allocation-free RTC breadcrumb for BLE
initialization, connection, passkey, authentication, handoff, and shutdown
checkpoints.

Every HTTP profile exposes `GET /api/pocket/v1/crash-report?offset=N`;
`/api/status` advertises `crashReportAvailable` and `crashReportBytes`. Each
request returns at most 1 KiB (160 bytes when the transient batch cannot be
allocated) and then yields to the physical-button loop; the screen preview uses
the same pattern with 4 KiB batches and a 1 KiB fallback.
Pocket reassembles the report, stores it in Application Support under a content
hash so reconnects do not create duplicates, classifies common memory, stack,
watchdog, and invalid-access failures, and offers both the raw report and export.
The bounded response replaces the old single-request stream whose blocking
socket write could trip the X3 task watchdog. The app independently persists its
last 80 CoreBluetooth transitions so discovery and pairing failures remain
inspectable even when the reader reboots before the HTTP handoff.

## Memory and responsiveness gates

Nearby Sync is not ready for release until measurements on both X3 and X4 demonstrate:

- BLE mode enters with at least 20 KiB minimum-ever free heap.
- NimBLE retains its supported 4 KiB host-task stack, and a second post-init
  gate requires 20 KiB free heap plus an 8 KiB contiguous block before pairing.
- BLE deinitialization returns enough contiguous heap to start the existing AP
  and web server with the current measured safety margin.
- No reader fonts, parsed books, AgentDeck sockets, mDNS, or Wi-Fi clients are
  alive while BLE is active.
- NimBLE initialization runs in a bounded worker, leaving button input and
  e-ink status rendering on the activity loop.
- BLE callbacks contain no filesystem, rendering, Wi-Fi, or blocking work.
- Back acknowledges immediately during radio startup and exits after the
  in-flight NimBLE initialization has safely unwound.
- The private-AP loop is enrolled in the task watchdog. A non-returning network
  handler therefore reboots into the retained crash-report screen instead of
  leaving an unresponsive e-ink frame indefinitely; its last HTTP/idle
  checkpoint is stored as the crash breadcrumb.
- Before starting HTTP, the minimal Pocket and X3 browser File Transfer
  profiles require 18 KiB free and an 8 KiB largest block. X3 File Transfer
  retains browser HTTP upload/download and settings while omitting the legacy
  WebSocket, WebDAV, UDP, mDNS, and captive-DNS allocations. The full X4 profile
  requires 20 KiB free and a 10 KiB largest block; allocation failures return
  to the UI rather than aborting the firmware.

If those gates cannot be met, the shipping fallback is still serverless:
`Create Hotspot` plus a one-tap app connection, with BLE disabled at build time.

## Apple app behavior

The App Store app owns only local state. It may download signed public content
and firmware from static release storage, but it does not require an account or
a user-operated server.

- The first iPhone/iPad slice provides Nearby discovery, secure hotspot handoff,
  connection verification, and file transfer. Content browsing and a share
  extension build on that transport.
- The first macOS slice provides the same transfer functions with automatic
  CoreWLAN association and a manual Wi-Fi join fallback. SD-card library management later uses the system folder picker
  and security-scoped bookmarks.
- A user-started transfer may finish in background time; periodic background
  execution is never presented as guaranteed.
