# Live Studio v1 — companion sync and UI pack contract

STATUS: DESIGN, agreed 2026-09-19. Nothing in this document is implemented
yet. This is the firmware side of a cross-repository contract; the companion
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

- **STA (File Transfer / Join a Network):** the existing `WebSocketsServer`
  (`src/network/CrossPointWebServer.cpp:332`) is enabled for a live-studio
  listener when `freeHeap >= 40 KB` at start. The port is advertised
  dynamically; the app never hardcodes it.
- **Private AP (POCKET_SYNC):** no WS listener. The app polls `/api/status`
  at 2 s while the studio is active and treats preview/live frames as
  unavailable unless heap allows the one-shot capture.
- **Legacy readers:** advertise nothing; the app keeps today's behavior.

## Event protocol (WS, JSON text frames)

Reader → app:

- `hello` — `{proto:"live-studio/1", deviceID, version, caps}` on connect.
- `status` — the full `/api/status` body, embedded verbatim under `status`.
  Sent when a stable field changes (identity, mode, capabilities; uptime,
  rssi, and freeHeap do not trigger a send) with a minimum 500 ms spacing,
  plus a 15 s keepalive that refreshes the live values.
- `frame` — `{seq, bytes, sha256}` notification only. The image body is
  fetched over HTTP (below), reusing the proven chunked transport instead of
  WS binary framing.
- `prefs` — `{changed:true}` after a preferences write from any client.
- `bye` — before an intentional server stop (e.g. mode change).

App → reader:

- `subscribe` — `{frames:bool, minIntervalMs}` (reader clamps to >= 250).
- `unsubscribe`.
- `ping` / `pong`.

All events are host-testable as pure encode/decode functions next to the
existing upload-stream protocol tests.

## Live frame capture and fetch

- New endpoint `GET /api/pocket/v1/screen-live?seq=<n>&offset=<o>` — chunked
  octet-stream paging identical to `screen-preview` (BMP, starts `0x42 0x4D`).
- Capture: after `displayBuffer()` completes, if a subscription is active,
  serialize the current 1-bit framebuffer under `RenderLock` into the
  transient BMP slot. A pure `LiveFramePolicy` (host-tested) gates capture:
  at least 10 KB free heap, at least 250 ms since the last capture, and a
  single-slot queue where a newer frame replaces an unfetched older one.
- The existing one-shot `screen-preview` contract is unchanged.

## UI pack format v1 (`.uipack`)

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
  state file `/.crosspoint/ui-pack.state` records
  `{name, packVersion, sha256}` and is written only after full validation.
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

The app consumes the host renderer as a binary artifact: this repository
builds `libpdui_host.a` + public header, and the app's
`scripts/sync_host_renderer.sh` copies them into `Support/PocketUIHost/`
with a `PROVENANCE.txt` recording the firmware commit SHA. This is a
deliberate, documented exception to "do not copy firmware binaries" — the
artifact is host-built, hash-pinned, and reviewed on both sides. No device
firmware source or image ever crosses.

## Budget and contingency

- Estimated firmware cost: WS listener + live capture + pack loader
  ≈ 25–40 KB flash. Current headroom is ~530 KB (map, build `c7480e45`).
- Contingency lever: builtin UI fonts are 2.43 MB in flash and
  `OMIT_FONTS` (`src/main.cpp:74`) already exists while SD fonts are fully
  supported — moving most builtin fonts to the SD bundle frees ~2 MB if the
  pack engine needs it.
- Heap rules: WS listener only on STA with >= 40 KB free; frame capture
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
