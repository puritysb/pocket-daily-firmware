# Pocket Reading Sync over BLE v1

Automatic, account-free exchange of reading places between a Pocket Daily reader
(X3/X4) and the Pocket Daily Apple app, without Wi-Fi modes or buttons. It reuses
the bonded Nearby Sync service (`nearby-sync-v1.md`) as its transport and the
reading-progress v1 records (`reading-progress-v1.md`) as its payload.

Status: contract for implementation (2026-09-29). Not yet hardware verified.

## Experience

- Close a book on the reader, and the phone already knows the place when the
  person next opens the app (it can arrive while the app is in the background).
- Read on the phone; the next time the reader wakes or returns Home, the phone's
  place is delivered, and opening that book offers "Pocket Daily iPhone: 52% · Go?"
  as today. Nothing ever moves a page without the person choosing it.
- It only works with a phone that was paired once through Nearby Sync (passkey).
  No new pairing is possible from an exchange window.

## Reader: exchange windows

The reader advertises the existing Nearby Sync service for a bounded **exchange
window** at natural moments, while Wi-Fi is off and no book is open:

| Trigger | Window |
| --- | --- |
| A book is closed (reader returns to Home/Library) | 45 s |
| Wake from sleep, after Home is drawn | 45 s |
| Power button / auto-sleep, after the sleep screen is drawn, before deep sleep | 20 s |

Rules:

- A window opens only when at least one bond exists, the setting "Sync places with
  your phone" is on (default on once a bond exists), battery is above 10 %, and the
  largest free heap block is at least `BLE_WINDOW_MIN_BLOCK` (measure on X3; start
  at 40 KB). Otherwise it is skipped silently.
- The window closes at once when a book is opened, any Wi-Fi mode or the Nearby Sync
  screen starts (which owns BLE), a button needs the heap, or the time is up. A
  sleep window then enters deep sleep. Radios are never on while a book is open.
- BLE and Wi-Fi remain mutually exclusive.
- Advertising uses the same service UUID and local name as Nearby Sync. Connections
  from a peer that is not already bonded are disconnected before pairing
  (`NimBLEDevice::isBonded`). All characteristics require encryption.
- `START_AP` is refused in a window with `ERR <id> NOT_IN_SYNC`; the hotspot still
  needs the Nearby Sync screen (physical presence).
- One connection at a time; after the exchange the phone disconnects and the reader
  keeps advertising until the window ends (another bonded phone may come).
- A small status line may show "Synced with iPhone" on Home; no modal UI.

## Status record

Unchanged format; `CAP` gains `READ1` when the reader supports this contract:

```text
V=1;MODEL=X3;ID=89ABCDEF;FW=1.7.1;CAP=AP,HTTP,SD,COMMIT1,READ1;WIN=1
```

`WIN=1` while in an exchange window, `WIN=0` on the Nearby Sync screen.

## Commands and events

Records keep the v1 limits (UTF-8, ≤ 220 bytes, newline-free, fields separated by
one space). `<id>` is eight uppercase hex digits chosen by the app. Payload chunks
are the last field and may contain spaces.

### Reading list (reader → app)

```text
app:    READ_LIST <id>
reader: D <id> <seq> <chunk>          (seq = 0,1,2,…; chunk ≤ 180 bytes)
reader: END <id> <total-bytes> <crc32-hex8>
```

The concatenated chunks are exactly the JSON body of
`GET /api/pocket/v1/reading` (reading-progress v1) **without** the `path` members,
so no file names cross the air. The app verifies length and CRC-32 (IEEE) before
parsing; on mismatch it discards the list. Errors: `ERR <id> BUSY|NO_MEMORY|FAILED`.

### Offer a place (app → reader)

```text
app:    OFFER <id> <total-bytes> <crc32-hex8>
app:    W <id> <seq> <chunk>          (write with response; seq = 0,1,2,…)
reader: OK <id>                        after the last chunk verified and stored
reader: ERR <id> UNKNOWN_DOCUMENT|BAD_RECORD|BAD_CHUNK|NO_MEMORY|BUSY
```

The body is exactly the JSON of `POST /api/pocket/v1/reading` and is handled by the
same validation and pending-place store. Total ≤ 1024 bytes. One offer at a time;
the app sends at most 10 offers per connection. A new `OFFER` or a disconnect
discards an incomplete one.

### Pacing

The reader sends `D` notifications from its activity loop, one per pass, retrying a
notification the stack could not queue. BLE callbacks only copy records (no SD, no
JSON, no allocation), as in v1.

## App (Apple platforms)

- After a successful Nearby Sync pairing the app remembers the peripheral identifier
  and the reader `ID` (never the passkey).
- iOS/iPadOS: a `CBCentralManager` with a restore identifier keeps a pending
  `connect` to that peripheral; the system completes it when the reader advertises,
  also while the app is suspended (`UIBackgroundModes: bluetooth-central`). macOS
  keeps the same pending connection while the app runs.
- On connect: discover the service, subscribe to events, read status, require
  `ID` = the remembered reader ID and `READ1` in `CAP`, then `READ_LIST`, merge
  through the same rules as the HTTP exchange (`docs/READING_PROGRESS.md` in the app
  repository), send offers for books that are further along on this device,
  disconnect, and re-arm the pending connection.
- The toggle "Your X3/X4 reader" in Continue Reading controls BLE and Wi-Fi exchange.
- Demo mode never connects.

## Privacy

Only the book fingerprint, the XPointer, the percentage and the device name cross
the link, over an encrypted bonded connection. File names are omitted. Nothing is
stored beyond what the HTTP exchange stores.
