# Pocket Reading Sync over BLE v1

Automatic, account-free exchange of reading places between a Pocket Daily reader
(X3/X4) and the Pocket Daily Apple app, without Wi-Fi modes or buttons. It reuses
the bonded Nearby Sync service (`nearby-sync-v1.md`) as its transport and the
reading-progress v1 records (`reading-progress-v1.md`) as its payload.

Status: contract (2026-09-29); reader side implemented 2026-09-29 on
`feat/ble-reading-sync`, host-verified only. Not yet hardware verified; the
memory figure below is provisional until measured on X3.

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
  (Not implemented in the first reader build.)

Reader implementation notes (2026-09-29):

- The setting is Settings → System → "Sync places with your phone"
  (`pocketReadingSync` in `settings.json`, default on). "At least one bond" is
  read from NimBLE's NVS store (`nimble_bond`, `peer_sec_<n>`) without starting
  the controller.
- Gates, in order: setting on, a bond stored, battery > 10 %, Wi-Fi off, free
  heap ≥ 64 KiB and largest block ≥ `BLE_WINDOW_MIN_BLOCK` before NimBLE starts
  (the free figure is the Nearby Sync screen's proven preflight), then free heap
  ≥ 20 KiB and largest block ≥ 8 KiB once it is up (else the window closes).
- Book-closed and wake windows start after the shell's next frame has been
  drawn (render counter; at most 5 s wait). The wake trigger is a power-button
  wake onto the shell (not a silent restart, crash, recovery or developer boot).
  A sleep trigger during an open window keeps it open for exactly 20 s more.
- The window closes before any screen other than Home, Pocket Daily, Library,
  Recent Books or the sleep screen is entered (a book, Settings, a dialog, a
  Wi-Fi mode, the Nearby Sync restart), so NimBLE's memory is back before that
  screen allocates. A button press during the sleep window ends it and the
  reader sleeps. NimBLE is fully deinitialized (`deinit(true)`) at every close.
- NimBLE starts in a worker task (the Nearby Sync screen's 6 KiB start task) so
  buttons stay responsive; a close during start waits for it to finish.
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

`BLE_WINDOW_MIN_BLOCK` = **40 KiB** (provisional; `ExchangeWindowPolicy.h`),
with a 64 KiB free-heap gate beside it. Not yet measured on X3/X4: this build
was not installed. Each window logs what the measurement needs:

```text
RSYNC window <trigger> starting (heap F, block B, battery P%)
NEARBY started window name=Pocket-XXXX heap=F0->F1 block=B0->B1
RSYNC window open for N ms (heap F, block B)
RSYNC List <id> sent: n books, m bytes      (or "Offer <id> stored")
NEARBY stopped heap=F2 block=B2
RSYNC window <trigger> skipped: <gate> (heap F, block B, battery P%)
```

`F0 − F1` is NimBLE init + advertising; the heap after a connection and an
exchange shows in `MEM` lines during the window; `F2` vs `F0` shows whether
deinit returned everything. Set the constant to the measured init cost plus
the exchange scratch (≈ 2.4 KiB list, ≈ 3.7 KiB offer, allocated only while an
exchange runs) plus margin, and record the figures here.

Resident cost: 32 bytes of static RAM; the service, record queue (4 × 220 B),
session and exchange buffers are allocated per window with nothrow allocation.

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
