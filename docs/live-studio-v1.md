# Live Studio v1 — companion status push and host renderer

STATUS: IMPLEMENTED (reduced 2026-10-01); design agreed 2026-09-19.
This is the firmware side of a cross-repository contract; the companion
application design lives in the sibling `pocket-daily` repository at
`docs/LIVE_STUDIO_DESIGN.md`. This document follows `docs/nearby-sync-v1.md`
as the second app/firmware contract.

What remains: a WebSocket status/preferences push on the STA File Transfer
listener, the `/api/status` `liveStudio` advertisement, the transfer-ownership
rules that keep optional services out of verified uploads, and the host
renderer (`host/`, `PocketUIHost`) the companion uses for local previews.
The reading path and the reader's own screens stay native.

## Removed 2026-10-01

The companion removed its `.uipack` encoder, theme-metric inspector,
reader-screen capture and live-frame fetching on 2026-09-25 (app `a98e879`).
The firmware side went on branch `refactor/remove-ui-packs`:

- UI packs (`ffe19edf`): the `.uipack` format, store and selection slots,
  UITheme's pack metric layering, `GET /api/pocket/v1/ui-packs`,
  `POST /api/pocket/v1/ui-pack/apply`, the `liveStudio.uiPacks`,
  `activePack` and `activePackVersion` status fields, the startup pack apply
  (`ProductBoot::loadPersistedState()` now loads only the Pocket profile), and
  the pack builder/field-registry scripts and tests.
- Live frame capture (`ad38b5db`): the render-task capture hook and
  `LiveFrameCapture`, the `canCaptureFrame` chain and screenshot guard,
  `GET /api/pocket/v1/screen-live`, the developer `dev/capture` and
  `dev/frame` routes, the WebSocket `frame` event and the
  `liveStudio.frameStream` field.
- Saved screen preview (`e5d6e79c`): the BMP saved before the Nearby Sync
  reboot, `GET /api/pocket/v1/screen-preview`, and the
  `screenPreviewAvailable`/`screenPreviewBytes` status fields.

The companion decodes all removed status fields as optional. Home, Daily
Brief and content previews come from the host renderer, not from device
frames. Older design text for these features is in the git history of this
file and in the dated entries of `docs/PROJECT_MEMORY.md`.

## Non-goals

- No app-composed EPUB rendering. The reader's layout and page cache stay
  authoritative.
- No automatic flashing. The staged `/update.bin` + on-device confirmation
  flow is unchanged.
- No WebSocket on the dedicated Sync profiles (heap); polling only.

## Transport

- **FULL / FILE_TRANSFER on STA (File Transfer → Join a Network):** push is
  allowed (`Profile.h::allowsLivePush`) when free heap is at least
  `LiveStudioEvents.h::kMinListenerFreeHeap` (16 KiB) as the server starts.
  FULL may still allocate its legacy WebSocket upload listener without push
  admission. A failed listener allocation leaves the reader in `poll` mode.
  Admission is not proof of sustainable runtime heap.
- **COMPANION (Pocket Sync → Join a Network) and POCKET_SYNC (private AP):**
  poll-only on X3 and X4. Neither allocates or resumes the listener, even
  with ample heap. The app polls `/api/status` at a paced interval.
- **Legacy readers:** no `liveStudio` object; the app keeps today's behavior.

The port is advertised dynamically in `/api/status`; the app never hardcodes
it.

### Transfer ownership (implemented 2026-09-21; hardware validation pending)

Pocket Sync → Join a Network selects the COMPANION profile on both X3 and X4:
status, verified upload/commit, preferences, content, profile and diagnostics.
It does not allocate the browser settings/file/font/OPDS/Wi-Fi management
route table, WebDAV, UDP discovery or mDNS. File Transfer retains its browser
routes. The root of app-only profiles returns status JSON. This is a resource
ownership boundary, not a measured heap saving. The upload stream creates
missing staging parent directories, so first-time uploads need no
browser-side mkdir.

The upload stream owns optional-service resources from socket acceptance
through HEADER, DATA and the REPLIED grace period (`TransferFocus`). The
listener is torn down when a transfer takes focus and may return only after
`TransferFocus::QUIET_MS` (5 s) without another upload and the same
`kMinListenerFreeHeap` check. While the port-82 stream or a legacy WebSocket
upload is mid-transfer the status push is suspended; that heap belongs to the
transfer. Status reports `mode:"push"` only while a listener actually exists.
HTTP is deferred during HEADER/DATA, with commit available during REPLIED.

`uploadStreamWindow:4096` in `/api/status` opts a client into flow control on
the port-82 stream. Send `Resume: 1` and `Window: 4096`, wait for `RESUME n`,
and send at most 4096 payload bytes. After writing a non-final block to SD and
updating the CRC, the reader replies `ACK <absolute offset>`; that reply grants
the next block. The final (possibly short) block gets the existing
`OK <size> <crc32>` instead. The receiver uses its existing borrowed staging
buffer; no new payload buffer is allocated. ACK describes an accepted SD write,
not a power-loss-durable journal. Resume state is RAM-only and resets on
reboot. A completed prefix is retained too, so a lost final OK can be retried
with `RESUME <size>` while the staging file still exists.

Never request a window from firmware that does not advertise it: old parsers
ignore unknown headers and would not send ACK. Old clients omit Window and
keep the original behavior. The Python delivery tool negotiates the same
extension and offers explicit pacing for bootstrapping old firmware.

The app cancels and drains preference/heartbeat requests before sending and
keeps them suspended through commit, then rechecks capability on its next
paced heartbeat. Socket failure clears the live client and a later healthy
heartbeat may restore it. CRC errors, SD errors, malformed responses and
identity changes remain terminal.

This bounds application payload in flight; it is not evidence that the Wi-Fi
driver's independent idle memory pressure is resolved. The heap floor and X3/X4
hardware acceptance gates still apply.

## Event protocol (WS, JSON text frames)

Every message is a single-key JSON object. The grammar is in
`src/pocket_daily/live_studio/LiveStudioEvents.{h,cpp}` (Arduino-free, host
tests in `test/live_studio/`); per-connection state is in
`src/pocket_daily/web/LiveStudioService.{h,cpp}`.

Reader → app:

- `hello` — `{proto:"live-studio/1", deviceID, version, caps:["status","prefs"]}`
  on connect.
- `status` — the full `/api/status` body, embedded verbatim under `status`.
  Sent on subscribe, then only when a stable field changes (version, STA/AP,
  device, device ID, upload-stream listening; uptime, rssi and freeHeap do not
  trigger a send), checked once per second with a minimum 500 ms spacing
  (`kMinStatusIntervalMs`). The event buffer is 1024 bytes; a status body that
  does not fit is not sent. No periodic keepalive: live values are the app's
  job to poll over HTTP, because a dead WS peer turns every queued send into a
  multi-second TCP retransmit stall on the reader's single loop. Sends go to
  the one subscribed client, after checking it is still connected.
- `prefs` — `{changed:true}` after a preferences write from any client.
- `bye` — best effort before an intentional server stop (e.g. mode change).

App → reader:

- `subscribe` — any body. Fields are ignored; an older companion's
  `{"frames":true,...}` is a plain status subscription.
- `unsubscribe`.
- `ping` → `pong`.

A text payload starting with `{` is always handled as a Live Studio control
message; anything else falls through to the inherited WebSocket upload
grammar. Unknown or malformed JSON decodes to nothing.

## `/api/status` advertisement

```
"liveStudio": {
  "mode": "push"|"poll",
  "wsPort": 81            // present only while the push listener runs
}
```

`mode` is always present on current firmware and the companion decodes it as
required. Absent `liveStudio` = legacy reader.

## Host renderer (exact preview)

`test/gfx_host` compiles the production GfxRenderer, bitmap/dither,
font/cache/decompression and MiniBidi sources against a bounded memory-display
HAL. A local font checker compares nonblank pixel buffers between Cached and
BoundedUI in eight geometry/orientation combinations, with regular/bold
Latin/Korean/Japanese text. Storage uses a host-only read-only asset HAL with
operation-scoped, thread-local bindings; callers own the immutable bytes and
serialize each renderer context; no OS paths are opened. Hardware
blit/grayscale methods fail explicitly. See `test/gfx_host/README.md` for
scope and reproducible commands.

The independent `host/` library exposes the `pdui_*` C ABI
(`host/include/PocketUIHost.h`, ABI version 1): content pages
(`pdui_render_content`), Home and Daily Brief layout previews
(`pdui_render_home`, `pdui_render_brief`, `pdui_render_sleep_brief`), the
companion's own cards (`pdui_set_cards`) and a glyph-fallback font
(`pdui_set_fallback_font`). Device and host run the same painters: content
pages go through `ContentPageRenderer`/`ContentImageRenderer`, which the device
reaches through `PocketDaily::Content::drawContentPage`/`drawContentImage`
(`src/pocket_daily/ContentPage.cpp`); Home and the Daily Brief go through
`src/pocket_daily/home/`. Contexts own copied font bytes; render calls are
serialized across contexts because MiniBidi shares static scratch. Failed
requests invalidate readback instead of exposing a stale or partial frame.
See `host/README.md`. Host/device pixel parity on physical panels is not
established.

### Cross-repository artifact (approved exception)

The app consumes the host renderer as a binary artifact. `host/build_apple.py`
packages `libpdui_host.a` and its public header/module map into
`PocketUIHost.xcframework` (macOS arm64/x86_64, iOS arm64, simulator
arm64/x86_64). Its `PROVENANCE.json` pins the commit plus actual dirty-tree
source hashes, compiler dependencies, SDK/build metadata and packaged file
hashes. A commit alone does not identify an uncommitted renderer. All five
slices link an actual Swift consumer.
The app `scripts/sync_host_renderer.sh` verifies this package and copies it
with its provenance into `Support/PocketUIHost/`. The app pins the accepted
source/artifact hashes, checks them during sandboxed builds and bundles metadata
for its runtime ABI check. This is a deliberate, documented exception to "do
not copy firmware binaries" — the artifact is host-built, hash-pinned, and
reviewed on both sides. No device firmware source or image ever crosses. Hash
provenance is not sender authentication.

### Home cover layout — 2026-09-30

Home uses a bounded 2:3 portrait cover slot in its left column in every Daily
Panel placement. Removing the panel adds space around the cover rather than
stretching its frame. The device fits the bitmap without cropping or changing
its aspect ratio and outlines the actual image; the host draws the shared
no-art illustration. Profile and ABI formats are unchanged. The companion must
refresh its pinned host artifact to preview this renderer; older installed
firmware retains its previous layout until explicitly updated.

### Sleep WAKE preview — 2026-09-30

The additive ABI v1 `pdui_render_sleep_brief` accepts `PDUI_SLEEP_WAKE_INDICATOR`
and `PDUI_SLEEP_BOOK_COVER`. It runs the same `PowerWakeCue` painter as the reader;
hardware and localized label are explicit inputs to that painter. The previous
`pdui_render_brief` preserves cover-on/cue-off behavior. Unknown option bits
invalidate the frame. Portrait Brief sections reserve the area below the wake
tab so it cannot obscure cover-free progress or a reordered section. The
companion follows the optional `sleepWakeIndicator`
preferences capability described in `nearby-sync-v1.md` and rerenders local
previews when either sleep preference changes. Reader-selected custom sleep
screens remain an outline; their private image is not fetched for preview.

## Safety review checklist

- The listener never allocates on a Sync profile, below
  `kMinListenerFreeHeap`, or during transfer focus.
- No push send while an upload owns the heap; a dead peer drops the
  subscription instead of queuing sends.
- Nothing in this protocol touches the OTA/update path or persists data.
