# Pocket Nearby Sync v1

Pocket Nearby Sync is the account-free link between the Pocket app and a
Xteink X3 or X4 running Crosspoint Pocket firmware. It is independent of AgentDeck and does not require
the phone and reader to be on the same infrastructure Wi-Fi network.

## Product contract

- The SD card is the source of truth for device content.
- The Apple app keeps a local mirror and can operate without a Pocket server.
- Bluetooth LE is a bounded control plane, not the bulk file transport.
- Existing CrossPoint HTTP, WebSocket, WebDAV, and SD-card formats remain the
  bulk and recovery paths.
- AgentDeck Dashboard is an optional provider and is not involved in pairing,
  content, learning state, or firmware updates.
- Deep sleep turns both radios off. Nearby Sync therefore starts only after the
  person wakes the device and explicitly opens the sync screen.

## Transport selection

The transport design selects the least expensive available link in this order:

1. Existing LAN connection discovered through `_crosspoint._tcp` Bonjour,
   with `crosspoint.local` and the direct-hotspot address as bounded fallbacks.
2. BLE for discovery, authenticated status, and a request to start a temporary
   device hotspot.
3. The temporary hotspot for files, fonts, content packs, screenshots, and
   firmware.
4. User-selected SD-card access on macOS as the offline and recovery path.

BLE and Wi-Fi are never kept active together on the no-PSRAM ESP32-C3. The
device sends the hotspot lease over the encrypted BLE connection, stops and
deinitializes BLE, and only then starts Wi-Fi AP mode.

## BLE service

Pocket advertises only while the Nearby Sync screen is visible.

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
V=1;MODEL=X3;ID=89ABCDEF;FW=1.4.1;CAP=AP,WS,SD
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
AP <request-id> <ssid> <passphrase> <ipv4> <http-port> <ws-port> <lease-seconds>
```

Fields never contain spaces. The v1 hotspot passphrase is 12 uppercase
hexadecimal characters. The app must treat it as ephemeral and must not sync it
to cloud storage or diagnostics.

The device sends `AP`, waits long enough for the notification to be delivered,
disconnects BLE, releases the Bluetooth controller memory, and starts the AP.
The app then joins the SSID with the platform hotspot configuration API and
verifies `GET /api/status` before transferring any data.

## Pairing and authorization

- BLE Secure Connections, MITM protection, and bonding are required.
- Pocket acts as display-only and shows a random six-digit passkey.
- The app asks the person to enter the displayed passkey.
- Entering Nearby Sync is the physical-presence authorization window. The
  device advertises for at most two minutes without a connection.
- A bonded client still cannot start a hotspot unless the Nearby Sync screen is
  currently open.
- `START_AP` is acknowledged only after the device has generated a new SSID,
  passphrase, and short lease.

## Hotspot and bulk transfer

Nearby Sync uses the existing CrossPoint bulk API unchanged:

- `GET /api/status` on port 80 verifies identity and mode.
- HTTP/WebDAV is used for metadata and small administrative operations.
- WebSocket port 81 uses `START:<filename>:<size>:<path>`, binary chunks,
  progress records, and `DONE` for files.
- Firmware and Pocket content packs keep their existing digest and atomic
  install checks; successful transport never implies successful installation.

The product hotspot is WPA2 protected, permits one client, and shuts down after
the lease or when the person leaves the transfer screen. Manual `Create Hotspot`
remains available for compatibility, but the app-assisted path uses per-session
credentials.

## Memory and responsiveness gates

Nearby Sync is not ready for release until measurements on both X3 and X4 demonstrate:

- BLE mode enters with at least 20 KiB minimum-ever free heap.
- BLE deinitialization returns enough contiguous heap to start the existing AP
  and web server with the current measured safety margin.
- No reader fonts, parsed books, AgentDeck sockets, mDNS, or Wi-Fi clients are
  alive while BLE is active.
- Button input is serviced on every activity loop iteration.
- BLE callbacks contain no filesystem, rendering, Wi-Fi, or blocking work.
- Back exits within 250 ms when no pairing or transition is in progress.

If those gates cannot be met, the shipping fallback is still serverless:
`Create Hotspot` plus a one-tap app connection, with BLE disabled at build time.

## Apple app behavior

The App Store app owns only local state. It may download signed public content
and firmware from static release storage, but it does not require an account or
a user-operated server.

- The first iPhone/iPad slice provides Nearby discovery, secure hotspot handoff,
  connection verification, and file transfer. Content browsing and a share
  extension build on that transport.
- The first macOS slice provides the same transfer functions with a manual Wi-Fi
  join fallback. SD-card library management later uses the system folder picker
  and security-scoped bookmarks.
- A user-started transfer may finish in background time; periodic background
  execution is never presented as guaranteed.
