# Pocket Reading Sync over BLE v1

Automatic, account-free exchange of reading places between a Pocket Daily reader
(X3/X4) and the Pocket Daily Apple app, without Wi-Fi modes or buttons. It reuses
the bonded Nearby Sync service (`nearby-sync-v1.md`) as its transport and the
reading-progress v1 records (`reading-progress-v1.md`) as its payload.

Current source (2026-10-05): the HTTP and BLE paths share `ReadingExchange`.
Lists advertise `offerVersion:2`; offers may bind to a positive `readerSeq`.
A changed reader position returns `STALE_POSITION`; legacy peers remain
forward-only. See `reading-progress-v1.md` for the common payload/storage rules.
The app distinguishes pairing, supported `READ1`, waiting, exchanging, errors,
and the last completed BLE exchange. Pairing alone is not sync support or proof
that a window opened. `/api/status.readSync` explains the reader's latest
memory/battery/bond gate only when HTTP status belongs to that paired reader.
The existing memory thresholds and radio ownership remain unchanged.
X3/X4 BLE availability, background delivery and handoff still need hardware
acceptance. The historical integration review is `ble-sync-review-2026-09-30.md`.

## Experience

- Closing a book opens an opportunity to deliver its place to the paired app,
  including in the background when the operating system allows it. Delivery
  depends on radio reachability, memory admission and the window duration.
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
| A book is closed (reader returns to Pocket Daily) | 45 s |
| Wake from sleep, after Pocket Daily is drawn | 45 s |
| Power button / auto-sleep, after the sleep screen is drawn, before deep sleep | 20 s |

Rules:

- A window opens only when at least one bond exists, the setting "Sync places with
  your phone" is on (default on once a bond exists), battery is above 10 %, and the
  free heap is at least 96 KiB and largest block is at least 40 KiB. These are
  provisional safety gates, not proof of availability on every X3/X4 shell.
  Otherwise it is skipped silently.
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
  (Not implemented in the first reader build.)

Reader implementation notes (2026-09-30):

- The setting is Settings → System → "Sync places with your phone"
  (`pocketReadingSync` in `settings.json`, default on). "At least one bond" is
  read from NimBLE's NVS store (`nimble_bond`, `peer_sec_<n>`) without starting
  the controller.
- Gates, in order: setting on, a bond stored, battery > 10 %, Wi-Fi off, free
  heap ≥ 96 KiB and largest block ≥ 40 KiB before NimBLE starts, then free heap
  ≥ 24 KiB and largest block ≥ 8 KiB once it is up. Running windows close below
  20 KiB free or 4 KiB largest block. List/offer admission and final offer parsing
  each require 24 KiB free / 8 KiB block; an offer returns `NO_MEMORY` otherwise.
- Book-closed and wake windows start after the shell's next frame has been
  drawn (render counter; at most 5 s wait). The wake trigger is a power-button
  wake onto the shell (not a silent restart, crash, recovery or developer boot).
  A sleep trigger during an open window keeps it open for exactly 20 s more.
- The window closes before any screen other than Pocket Daily or the sleep
  screen is entered (a book, stock CrossPoint Home/Library/file browser,
  Settings, a dialog, a Wi-Fi mode, the Nearby Sync restart). Stock screens
  never host a window (2026-10-01): a book read from Home syncs at the next
  sleep or wake window instead. Teardown precedes `onEnter`; activity
  constructors may already have run. Library replaces the removed Recent Books
  screen in the 1.6.5 baseline. Book-close arming waits for all pending navigation
  to finish, including Pop-to-Home and returning to a stacked shell. A button press during the sleep window ends it and the
  reader sleeps. NimBLE is fully deinitialized (`deinit(true)`) at every close.
- NimBLE starts in a worker task (the Nearby Sync screen's 6 KiB start task) so
  buttons stay responsive; a close during start waits up to 15 s for it to finish.
  If initialization never returns, the reader restarts for recovery rather than
  releasing worker-owned memory or handing a still-active radio to Wi-Fi.
- In a window the reader uses no-input/no-output IO capability with bonding off
  and MITM required, so no pairing can succeed or store anything (a stranger can
  never evict the person's bond) and no passkey exists. A connecting peer whose
  identity address is not bonded is disconnected in `onConnect`; a peer with an
  unresolved resolvable private address gets 5 s to re-encrypt with its bond.
  Commands are accepted only on an encrypted, authenticated, bonded link.
- Advertising continues after a disconnect until the window ends.
- READ_LIST / OFFER / W are also served on the Nearby Sync screen (`WIN=0`),
  with the same code.

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

### Wire details (clarified 2026-09-29, reader implementation)

These make explicit what the reader does; none changes the grammar above.

- `END` carries the CRC as eight **upper-case** hex digits (`END 0000001A 812
  0A1B2C3D`); the reader accepts either case in `OFFER`.
- `OFFER` itself has no reply. The single reply to an offer is `OK <id>` after
  its last chunk, or one `ERR <id> <code>`; later `W` chunks of a failed offer
  are ignored silently.
- Offer error codes: `BAD_RECORD` (total 0 or > 1024, JSON rejected by the
  shared validation, or `deviceID` not this reader), `BAD_CHUNK` (seq not the
  next one, overrun, CRC mismatch, or a `W` for no announced offer),
  `UNKNOWN_DOCUMENT` (no recent book has that `document`/`filenameDocument`),
  `NO_MEMORY`, `BUSY` (a list is streaming, or a chunk was dropped because the
  reader's 4-record queue was full), and additionally **`FAILED`** when the
  pending place could not be written to the SD card (HTTP answers 500). Apps
  should treat unknown codes as a failed offer.
- `W` chunks are raw bytes of the body: a chunk may split a multi-byte UTF-8
  character and may begin or end with a space. Records must not contain control
  characters (< 0x20, 0x7F); JSON escaping already guarantees that. A chunk is
  at most 220 − len(`W <id> <seq> `) bytes.
- Errors outside an exchange: a record without a valid id → `ERR 00000000
  BAD_COMMAND`; an unknown verb → `ERR <id> UNKNOWN_COMMAND`; bad arguments to
  `PING`/`CANCEL`/`READ_LIST` → `ERR <id> BAD_COMMAND`.
- `D` records start at seq 0 and are sent in order; the list is pure ASCII
  (digests, XPointers, numbers) because paths are omitted.
- A new connection discards any incomplete list or offer and any record still
  queued from the previous connection.

### Pacing

The reader sends `D` notifications from its activity loop, one per pass, retrying a
notification the stack could not queue. BLE callbacks only copy records (no SD, no
JSON, no allocation), as in v1.

## Memory

The same figures are kept in RTC_NOINIT memory (`nearby_sync/ReadSyncStats`) and
reported by `GET /api/status` as `readSync` once any window was attempted since
power-on: counts (`opened`, `skipped`, `refused`, `connections`, `lists`,
`offers`), the last `lastTrigger`/`lastGate`/`lastClose`, and heap/largest block
before NimBLE (`startFree`/`startBlock`), once advertising (`openFree`/`openBlock`),
the lowest while open (`minFree`/`minBlock`) and after deinit
(`closedFree`/`closedBlock`). They survive the restart into a Wi-Fi mode, so a
reader put into Same Wi-Fi after a test shows the last window.

The former X3 test (2026-09-29, older baseline) measured approximately 82.9 KB
free / 77.8 KB block before NimBLE, 13.7 KB / 11.3 KB while advertising, and
6.9 KB / 2 KB at the low point. The phone did not complete an exchange. These
figures do **not** validate the current SDK, a bonded exchange or safe headroom.
The temporary 10/6 KiB ready and 6 KiB running floors have been removed.

Current provisional gates are 96/40 KiB before startup, 24/8 KiB after startup,
and 20/4 KiB while running (free/largest block). Thus the historical 82.9 KB Home
case is deliberately skipped. Measure current X3 and X4 before claiming automatic
sync is available there; freeing idle shell resources may be necessary. Do not
reduce the gate simply to obtain an advertisement.

Inspect `readSync.startFree/startBlock`, `openFree/openBlock`,
`minFree/minBlock`, `closedFree/closedBlock`, and completed `lists/offers`.
The init delta includes the temporary startup worker; no-peer advertising is not
a substitute for a real exchange. Low-memory skip/close paths avoid extra serial
logging and keep RTC statistics for later HTTP inspection.

Service, four-record callback queue, session and exchange scratch are allocated
only for a window. List scratch is about 2.4 KiB; an offer plus store scratch is
about 3.7 KiB, in addition to transient JSON/SD/radio allocations. These existing
nothrow allocations keep stack usage bounded and are released at session end;
this review adds no permanent heap reservation.

## App (Apple platforms)

- After a successful Nearby Sync pairing the app remembers the peripheral identifier
  and the reader `ID` (never the passkey).
- iOS/iPadOS: a `CBCentralManager` with a restore identifier keeps a pending
  `connect` to that peripheral with `UIBackgroundModes: bluetooth-central`.
  Background execution/restoration is OS controlled, not a delivery guarantee.
  macOS keeps the pending connection while the app runs. See Apple
  [Core Bluetooth background processing](https://developer.apple.com/library/archive/documentation/NetworkingInternetWeb/Conceptual/CoreBluetooth_concepts/CoreBluetoothBackgroundProcessingForIOSApps/PerformingTasksWhileYourAppIsInTheBackground.html).
- On connect: discover the service, subscribe to events, read status, require
  `ID` = the remembered reader ID and `READ1` in `CAP`, then `READ_LIST`, merge
  through the same rules as the HTTP exchange (`docs/READING_PROGRESS.md` in the app
  repository), send offers for books that are further along on this device,
  disconnect, and re-arm the pending connection.
- The toggle "Your X3/X4 reader" in Continue Reading controls BLE and Wi-Fi exchange.
  "Forget this reader" removes the app's remembered identity and pending connection;
  it does not erase the OS or reader bond. Disable the reader setting too to stop
  its automatic windows while that bond remains.
- Demo mode is bound synchronously before the app starts the central. Explicit
  Nearby Sync ownership also cancels the automatic connection synchronously.
  The existing app HTTP/user-work lane blocks BLE until its I/O drains, including
  quiet HTTP exchanges, so both transports cannot overlap the shared merge state.
- A list `END` may arrive before the `READ_LIST` write acknowledgement; the app
  waits for both before offering. Each following write has its own acknowledgement.
  Only persisted `OK` replies count as sent; failed or unknown-document offers do not.
- Local reading-sync diagnostics retain phase/count/error information, without
  raw status records or reader/peripheral identifiers.

## Privacy

Only the book fingerprint, the XPointer, the percentage and the device name cross
the link, over an encrypted bonded connection. File names are omitted. Nothing is
stored beyond what the HTTP exchange stores.

## Verification

Host (`test/reading_sync_ble`): record grammar and limits, CRC-32 check value,
D chunking, offer reassembly, the callback queue, window gates and lifecycle
(triggers, render wait, early closes, sleep extension, failed starts), and
READ_LIST/OFFER end to end through the real record half of the service, the
session and the exchange shared with the HTTP routes on a fake SD card (list =
HTTP body minus paths, byte-exact; offers stored exactly as over HTTP; error
codes; retry of refused notifications; link changes; queue overflow).
`scripts/test_sync_routes.py` pins that HTTP and BLE share the exchange code.

Needs X3/X4 with a paired iPhone: the heap figures above; bonded reconnect in a
window (RPA resolution) and refusal of an unbonded phone without any passkey or
bond change; each trigger (book closed, wake, sleep) and each early close;
opening a book right after closing one (window start in flight); that sleep
still happens and wake works after a sleep window; battery cost of windows; and
the app's background reconnect.
