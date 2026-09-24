# Live Studio v1 — companion sync and UI pack contract

STATUS: PARTIALLY IMPLEMENTED; design agreed 2026-09-19, audited 2026-09-22.
This is the firmware side of a cross-repository contract; the companion
application design lives in the sibling `pocket-daily` repository at
`docs/LIVE_STUDIO_DESIGN.md`. This document follows `docs/nearby-sync-v1.md`
as the second app/firmware contract.

Goal: the companion app becomes a live studio for the reader — real-time
device state, a live exact-frame preview, and UI/theme definition packs that
are composed and previewed in the app, deployed over the existing verified
transfers, and applied on the reader without reflashing. The
performance-critical reading path stays native; only "chrome" becomes
definition-driven.

## Non-goals

- No app-composed EPUB rendering. The reader's layout and page cache stay
  authoritative.
- No automatic flashing. A UI pack is data, never firmware; the existing
  staged `/update.bin` + on-device confirmation flow is unchanged.
- No WebSocket on the private AP profile in v1 (heap); polling fallback only.

## Transport

Dedicated-session update (2026-09-24): COMPANION is now poll-only like
POCKET_SYNC on both X3 and X4. Neither profile allocates or resumes the WS
listener, even with ample heap. Status suppresses automatic preview/crash
advertisements and their SD probes; explicit diagnostic handlers remain.
Content/theme application and revision-bound redraw receipts are unchanged.
This supersedes COMPANION push admission below; File Transfer retains its
existing policy. No new buffer/task or wire schema, and no physical stability
claim until both bearers pass the companion SYNC_SESSIONS.md checklist.
The theme list/apply routes are now independent of the optional frame route:
private AP previously advertised uiPacks but did not register these handlers.
Both dedicated bearers now register them, guarded by SyncRouteBoundaries.

### Transfer ownership (implemented 2026-09-21; hardware validation pending)

Pocket Sync → Join a Network now selects a separate COMPANION profile on both
X3 and X4: status, verified upload/commit, preferences, diagnostics and Live
Studio/UI packs. It does not allocate the browser settings/file/font/OPDS/Wi-Fi
management route table, WebDAV, UDP discovery or mDNS. Existing File Transfer
retains its browser routes. The root of app-only profiles returns status JSON.
This is a resource ownership boundary, not a measured heap saving yet.
The stream creates missing staging parent directories so first-time UI-pack
and learning uploads do not require a browser-side mkdir. UI-pack loading checks
the minimum header size before inspecting SHA fields in an SD file.

The upload stream owns optional-service resources from socket acceptance
through HEADER, DATA and the REPLIED grace period. Every terminal path releases
focus after file/socket cleanup; the WS listener may return only after five
seconds without another upload and the existing heap admission check. Status
advertises push only while an actual listener exists. HTTP is deferred during
HEADER/DATA, with commit available during REPLIED. The current local tree has
removed gateway-probe-driven reconnects in favor of passive association tracking;
the frozen installed `w998f2f9d` baseline still has the older probe ladder.
Do not conflate the local correction with installed or hardware-proven behavior.

`uploadStreamWindow:4096` in `/api/status` opts a client into flow control on
the existing port-82 stream. Send `Resume: 1` and `Window: 4096`, wait for
`RESUME n`, and send at most 4096 payload bytes. After writing a non-final block
to SD and updating the CRC, the reader replies `ACK <absolute offset>`; that
reply grants the next block. The final (possibly short) block gets the existing
`OK <size> <crc32>` instead. The receiver uses its existing borrowed staging
buffer; no new payload buffer is allocated. ACK describes an accepted SD write,
not a power-loss-durable journal. Resume state remains RAM-only and resets on
reboot. A completed prefix is retained too, allowing a lost final OK to be retried
with `RESUME <size>` while the staging file still exists.

Never request a window from firmware that does not advertise it: old parsers
ignore unknown headers and would not send ACK. Old clients omit Window and
retain the original behavior. The Python delivery tool negotiates the same
extension and offers explicit pacing for bootstrapping old firmware.

The app cancels and drains frame/preference/heartbeat requests before sending,
keeps them suspended through commit and pack apply, then rechecks capability on
its next paced heartbeat. Socket failure clears the live client and a subsequent
healthy heartbeat may restore it. LAN recovery waits up to 45 seconds in spaced
probes and compares the original device ID before resuming. CRC errors, SD
errors, malformed responses and identity changes remain terminal.

This bounds application payload in flight; it is not evidence that the Wi-Fi
driver's independent idle memory pressure is resolved. The heap floor and X3/X4
hardware acceptance gates still apply.

- **STA (File Transfer / Join a Network):** the current live-push admission
  threshold is 16 KiB (`LiveStudioEvents.h::kMinListenerFreeHeap`), not the
  earlier 12/40 KB draft values. FULL File Transfer may still allocate its
  legacy WS listener without live-push admission; COMPANION does not. Admission
  is not proof of sustainable runtime heap. While an upload is in progress the status push
  is suspended — that heap belongs to the transfer. The port is advertised
  dynamically; the app never hardcodes it.
- **Private AP (POCKET_SYNC):** no WS listener. The current app polls `/api/status`
  at a paced 15 s interval and treats preview/live frames as
  unavailable unless heap allows the one-shot capture.
- **Legacy readers:** advertise nothing; the app keeps today's behavior.

## Event protocol (WS, JSON text frames)

Reader → app:

- `hello` — `{proto:"live-studio/1", deviceID, version, caps}` on connect.
- `status` — the full `/api/status` body, embedded verbatim under `status`.
  Sent only when a stable field changes (identity, mode, capabilities;
  uptime, rssi, and freeHeap do not trigger a send) with a minimum 500 ms
  spacing. No periodic keepalive: live values are the app's job to poll over
  HTTP, because a dead WS peer turns every queued send into a multi-second
  TCP retransmit stall on the reader's single loop.
- `frame` — `{seq, bytes}` notification only. The image body is
  fetched over HTTP (below), reusing the proven chunked transport instead of
  WS binary framing. A per-frame content hash was deliberately left out of
  v1: integrity is transport-level (TCP checksums) plus the app-side BMP
  validation, and hashing would re-read the frame from SD on every capture.
  The encoder emits unsigned32-bit integers. The app rejects boolean/fractional/
  negative or out-of-range sequence values and sizes outside its existing
  64..131072-byte frame-fetch bound before scheduling a request (2026-09-22).
  Sequence zero is valid after counter wrap; these checks do not change the wire layout.
- `prefs` — `{changed:true}` after a preferences write from any client.
- `bye` — before an intentional server stop (e.g. mode change).

App → reader:

- `subscribe` — `{frames:bool, minIntervalMs}` (reader clamps to >= 250).
- `unsubscribe`.
- `ping` / `pong`.

All events are host-testable as pure encode/decode functions next to the
existing upload-stream protocol tests.

## Live frame capture and fetch

- New endpoint `GET /api/pocket/v1/screen-live?offset=<o>` — chunked
  octet-stream paging identical to `screen-preview` (BMP, starts `0x42 0x4D`),
  reading the latest captured frame (single slot; latest wins). Registered on
  the STA profiles alongside the WS listener.
- **Current client discipline:** serialize HTTP requests per host, coalesce
  frame announcements and space frame fetch starts by at least one second;
  cancel/drain optional requests before a user transfer. The current Arduino
  WebServer emits `Connection: close` and releases ordinary clients after their
  handler. URLSession therefore cannot guarantee persistent reuse across pages.
  The earlier single-persistent-connection requirement was not implemented;
  a bounded single-response frame path is still future work. Do not interpret
  historical radio stalls as proof that connection churn is the sole cause.
- Capture: after each completed render pass on the render task
  (`ActivityManager::renderTaskLoop`), if a frames subscription is active,
  the current 1-bit framebuffer is written row-by-row to
  `/.crosspoint/live-frame.bmp` (no framebuffer-sized allocation). The pure
  policy (`shouldCaptureFrameAt`: at least 10 KB free heap, subscription
  spacing clamped to >= 250 ms, wrap-safe math) is host-tested. A single-slot
  flag hands the notification to the web server's activity loop, which sends
  the `frame` event (worst-case notification latency is the 1 s server tick).
- The existing one-shot `screen-preview` contract is unchanged.

Content presentation capture guard (2026-09-22): render-pass return alone is not
success. ActivityManager consults the activity's `canCaptureFrame` under the
render lock. The File Transfer host refuses queued/failed content frames and
requires a completed native paint after a failed view is closed. Manual
screenshots use the same guard before saving or displaying their border. Failed
content paint leaves any prior valid live-frame file/sequence unchanged, rather
than publishing partially drawn pixels. The protocol layout is unchanged; a
prior capture remains historical, not proof of the current requested revision.
Controller transitions have host tests; physical capture parity is still pending.

## UI pack format v1 (`.uipack`)

Activation implementation update (2026-09-22): stored pack basenames are limited
to 1–24 ASCII letters/digits/hyphen/underscore; embedded name must match the
basename, without truncation. Version is 1–16 ASCII letters/digits/dot/hyphen/
underscore. The app publishes a unique `studio-<16-character revision>.uipack`
instead of overwriting the file backing the active pack.

Selection now alternates between `/.crosspoint/ui-pack.state.0` and `.1`.
Each is exactly 64 bytes: `PDS1` at 0, little-endian generation at 4,
NUL-padded name[25] at 8, version[17] at 33, zero padding through byte 59,
and little-endian CRC32 of bytes 0–59 at 60. Generation zero is invalid;
overflow is rejected. Empty name and version are an explicit revert tombstone.
The newest valid generation wins; equal generations prefer slot 0. A new write
preserves the currently active selection slot (including a boot fallback),
checks byte count, flushes/closes and verifies readback before runtime activation.
The old text state is read for migration only when neither slot is valid and is
removed after the first verified new record.

Boot validates name/version and the referenced pack, falling back to the older
valid selection if the newest pack cannot be loaded. An explicit newest revert
does not revive an old pack. Status reports actual runtime activation rather
than an unvalidated on-disk selection. Loaded override storage is optionally
shrunk before commit, then adopted without allocation under RenderLock; failed
state writes retain the old runtime metrics. Each pack is rebuilt over native
theme metrics, not over the previous pack, and revert restores that base.

Streaming validation update (2026-09-22): the SD loader no longer allocates the
entire pack. A shared callback parser reads at most 128 bytes at a time, computes
CRC and optional SHA through one payload pass, then reads bounded record headers.
The caller must keep the file stable during both passes. The device's existing
8 KiB limit and rejection of embedded font assets remain; this is not larger-pack
or font support. Read/digest failures and all truncated lengths are host-tested.

CRC/truncation/selection and metrics composition have host tests, not physical
power-cut sign-off. SD/FAT-wide damage is not solved by two files. Multi-file
resource transactions, authenticated product writes
and bounded retention/garbage collection are still pending.

Implementation correction (2026-09-21): the companion encodes each theme
record with its registry type (1=int, 2=bool, 3=float) and a four-byte value;
bool uses 0/1 and float uses IEEE-754 Float32 bits. All theme records are seven
bytes, including bool. The header is 120 bytes. Swift and firmware host tests
pin the same complete mixed-type fixture. The app verifies reader identity
and exact active pack/version after apply, and nil pack/version after revert;
a failed status request is not reported as successful activation. This is not
physical UI-pack or power-loss transaction sign-off.

Value validation update (2026-09-22): the reader parser now rejects noncanonical
bool words (anything except 0/1) and every Float32 infinity/NaN encoding, matching
the companion encoder. CRC-valid malformed values are still rejected. Finite
float acceptance alone is only format validation. Both encoder and parser now
also require `popupTopOffsetRatio` in [0, 1] (signed zero accepted) and
`homeCoverHeight` in [1, 2048] pixels. The latter is a pack resource bound,
not a hardcoded panel dimension; zero would reach a cover-ratio divisor.
These restrictions do not change the binary layout, but older packs outside
these domains are rejected rather than clamped. Existing native theme defaults
fit these domains. Other field bounds and composed-layout safety remain open.

Renderer binding update (2026-09-22): Base, Lyra, Lyra 3 Covers and RoundedRaff
rendering no longer read their compiled `*Metrics::values` directly. Those
defaults still select the native base; drawing reads the active UITheme metrics
so existing metric consumers see the same pack as layout callers. Button sizes
previously held as constexpr locals are now runtime values. A source-boundary
CTest prevents direct constant reads from returning; it is not a pixel test.
Pagination uses bounded positive divisors/counts, including tiny viewports and
zero/negative/overflowing row steps. This does not validate arbitrary drawing
coordinates or guarantee every theme implements every registered field (e.g.
some RoundedRaff dimensions remain font-derived). Host/device pixel parity,
field-specific bounds and physical apply/revert remain pending.

A UI pack overrides chrome appearance only. It follows the learning/font pack
pattern: fixed header, hash-validated, atomically installed, state-file
recorded.

```
Header (fixed):
  magic[4]       "PDUI"
  version        u8 = 1
  reserved       u8 x3 (zero)
  name           char[32]
  packVersion    char[16]
  minFirmware    char[16]   // reader CROSSPOINT_VERSION prefix check
  themeOverrides u16
  stringCount    u16
  assetCount     u16
  reserved       u16
  payloadLen     u32
  crc32          u32        // over payload
  sha256         u8 x32     // over payload

ThemeOverride record: fieldId u16, type u8 (1=int,2=bool,3=float),
                      value s32/u8/f32
String record:        langIndex u8, keyHash u32 (FNV-1a of i18n key),
                      len u16, utf8 bytes (<= 128)
Asset record (v1: fonts only): kind u8 = 1 (.cpfont), nameLen u8, name,
                      size u32, crc32 u32, bytes
```

- **ThemeMetrics field-id registry:** stable numeric ids for the ~100
  `ThemeMetrics` fields (`src/components/themes/BaseTheme.h:26`), generated
  into a shared header consumed by the pack builder (firmware repo script)
  and the host renderer. Positional/implicit ordering is forbidden; packs
  carry ids, the loader validates against a whitelist.
- **Validation:** magic/version, bounded counts, whitelisted field ids and
  types, string length bounds, cpfont validation for font assets, then CRC32
  and SHA-256. A pack that fails any check is never installed or applied.
- **Install:** uploaded through the port-82 stream to `.part` staging,
  CRC-verified, committed to `/pocket-daily/ui-packs/<name>.uipack`. The
  CRC/generation selection slots described above record the basename/version
  after pack validation. File-content integrity comes from the pack's validation;
  the selection record itself does not contain the pack SHA-256.
- **Apply / revert:** `POST /api/pocket/v1/ui-pack/apply` with `{name}` (or
  empty to revert). Apply re-validates, swaps the active metrics source,
  and calls `UITheme::reload()` so the current screen re-renders with the
  new metrics. Draw methods keep reading through `UITheme`'s metrics
  pointer; today they read `currentMetrics = &<Theme>::values` (constexpr
  statics, `src/components/UITheme.cpp:16-40`) — the loader adds a
  pack-populated `ThemeMetrics` instance as an alternative source.
  Implementation must first verify every theme reads metrics through that
  pointer (no captured references) and fix any that do not.
- **String overrides** apply to chrome strings only, never the reader, and
  never widen the compiled i18n key set.

## `/api/status` advertisement

```
"liveStudio": {
  "wsPort": 81,            // present only when the listener is running
  "mode": "push"|"poll",
  "frameStream": true|false,
  "uiPacks": true|false,
  "activePack": "name"|null,
  "activePackVersion": "x.y"|null
}
```

Absent `liveStudio` = legacy reader; the app falls back to today's behavior.

## Host renderer (exact preview)

Implementation foundation (2026-09-22): `test/gfx_host` now compiles the production
GfxRenderer, bitmap/dither, font/cache/decompression and MiniBidi sources against
a bounded memory-display HAL. A local font checker compares nonblank pixel
buffers between Cached and BoundedUI in eight geometry/orientation combinations,
with regular/bold Latin/Korean/Japanese text. This also exposed and fixed layout
whitespace incorrectly treated as missing glyphs during content preflight.
Storage now uses a host-only read-only asset HAL, with operation-scoped,
thread-local bindings instead of the mutable global fake SD. Callers own the
immutable bytes and serialize each renderer context; no OS paths are opened.
Hardware blit/grayscale methods fail explicitly. The independent `host/` library
now exposes a content-page C ABI and an Apple XCFramework builder. The named
native theme surfaces and physical goldens below remain incomplete. The app
now imports the verified XCFramework and has an actor-isolated content bridge;
preview UI and production font selection remain pending.
See `test/gfx_host/README.md` for scope and reproducible commands.

Content-page extension: device BaseTheme and host now call the same
ContentPageRenderer for title/body/footer and image placement, and the same
ContentImageRenderer for PBM rasterization and failure clearing.32 real-font
text/card/empty/image frames compare Cached/BoundedUI pixel-identically. Separate
image pixel-oracle and late-read-failure tests cover both geometries/all four
orientations. The C ABI additionally matches24 direct page frames and has an
executed macOS Swift import check. Contexts own copied font bytes; render calls
are serialized across contexts because MiniBidi shares static scratch. Failed
requests invalidate readback instead of exposing a stale/partial frame. See
`host/include/PocketUIHost.h` and `host/README.md`. Other named surfaces,
UI-pack application through the ABI and the app preview UI remain pending.

- New host build target compiles `GfxRenderer`, the theme sources, and
  `EpdFont` against an `HalDisplay` stub that renders into a memory
  framebuffer (the renderer's only hardware coupling is the `HalDisplay`
  facade; `EpdFont.cpp` already builds on host).
- A small C ABI (`pdui_*`) exposes: context create, apply pack bytes, render
  a named chrome surface with a synthetic state document, read back the 1-bit
  buffer, destroy. Surfaces in v1: `home`, `settings_main`,
  `pocket_overview`, `confirm_dialog`.
- **Golden tests:** the same pack + synthetic state render both on host and
  on device (a dev-build-only `GET /api/pocket/v1/golden?surface=` returns
  the device-rendered buffer hash); a host test byte-compares. This is the
  drift guard between preview and device.

### Cross-repository artifact (approved exception)

The app consumes the host renderer as a binary artifact. `host/build_apple.py`
now packages `libpdui_host.a` and its public header/module map into
`PocketUIHost.xcframework` (macOS arm64/x86_64, iOS arm64, simulator arm64/x86_64).
Its `PROVENANCE.json` pins the commit plus actual dirty-tree source hashes,
compiler dependencies, SDK/build metadata and packaged file hashes. A commit
alone does not identify an uncommitted renderer. All five slices link an actual
Swift consumer; iOS execution and app integration are not implied.
The app `scripts/sync_host_renderer.sh` verifies this package and copies
it with its provenance into `Support/PocketUIHost/`. The app pins the accepted
source/artifact hashes, checks them during sandboxed builds and bundles metadata
for its runtime ABI check. Simulator tests exercise the actual bridge, not a
mock renderer. Preview UI and physical parity remain pending. This is a
deliberate, documented exception to "do not copy firmware binaries" — the
artifact is host-built, hash-pinned, and reviewed on both sides. No device
firmware source or image ever crosses. Hash provenance is not sender authentication.

## Budget and contingency

- Historical estimate: WS listener + live capture + pack loader
  ≈ 25–40 KB flash; build `c7480e45` had ~530 KB headroom. These are not
  measurements of the current dirty tree or installed `w998f2f9d`.
- Contingency lever: builtin UI fonts are 2.43 MB in flash and
  `OMIT_FONTS` (`src/main.cpp:74`) already exists while SD fonts are fully
  supported — moving most builtin fonts to the SD bundle frees ~2 MB if the
  pack engine needs it.
- Heap rules: live-push admission on STA requires >= 16 KiB free (FULL's legacy
  WS exception described above); frame capture
  gated by `LiveFramePolicy` (>= 10 KB); pack validation runs only when
  idle, never mid-transfer.

## Phased delivery

- **LS-1 (firmware):** WS listener + event protocol + `/api/status`
  advertisement; host tests for event encode/decode and `LiveFramePolicy`.
- **LS-2 (firmware):** `screen-live` capture/fetch with throttling; physical
  X3 sign-off for render-path safety (no watchdog resets while reading).
- **LS-3 (firmware):** `.uipack` loader/validator/apply + metrics source
  swap + field-id registry generator + pack builder script; golden-surface
  dev endpoint.
- **LS-4 (both):** host renderer C ABI + artifact sync + golden image
  parity; app-side studio consumes it.

Each phase lands with its companion app change verified in the same effort
per the repository-boundary rule.

## Safety review checklist

- A malformed pack can never reach the metrics pointer or i18n tables
  (whitelist + bounds + hash before apply).
- Frame capture must not stall the render task beyond one `RenderLock`
  window; `LiveFramePolicy` refuses under heap pressure.
- No pack operation touches the OTA/update path or persists anything before
  validation.
