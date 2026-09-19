# Pocket Daily Firmware Project Memory

This is concise, repository-owned context for future sessions. It is not a chat
transcript. Current source and release records override dated observations.

## Repository split

- Firmware: this repository (`https://github.com/puritysb/pocket-daily-firmware`),
  checked out on this host at `/Users/puritysb/git/pocket-daily-firmware`
- App Store app: sibling directory `pocket-daily` next to this clone
  (`https://github.com/puritysb/pocket-daily`), on this host at
  `/Users/puritysb/git/pocket-daily`

The host checkout root moved from `~/github/` to `~/git/` (noted 2026-09-19).
Absolute `~/github/` paths in either repository's older notes are stale;
resolve the sibling repository relative to this checkout.

The firmware repository owns device behavior, endpoints, local persistence,
memory gates, and flashing. The app repository owns the Apple-platform client.
Nearby Sync v1 is a contract between them; do not change only one side.

The old combined repository and its generated memories used the path
`crosspoint-agentdeck` and mixed firmware, AgentDeck, and app concerns. Do not
copy those memories wholesale. Promote only a verified, firmware-relevant fact.

## Git topology

- `origin`: `puritysb/pocket-daily-firmware`
- Product branch: `main`
- `upstream`: `crosspoint-reader/crosspoint-reader`
- Upstream foundation branch: `upstream/master`

Pocket Daily product directories are intentionally downstream-only. Use the
repository sync script and merge upstream; never rebase the product history.

## Durable release constraints

- X3 and X4 are no-PSRAM ESP32-C3 devices.
- `docs/nearby-sync-v1.md` defines mandatory runtime heap and responsiveness
  gates for both models.
- A host build and host unit tests are necessary but do not satisfy hardware
  sign-off.
- `firmware/LATEST_BUILD.txt` identifies the locally staged ignored artifact.
- GitHub releases are the production OTA source; `firmware.bin` must be present
  as an exact release asset name.
- App Store timing is independent, but Nearby Sync v1 must be frozen and
  compatible before either side is presented as production-ready.

## Exact companion preview contract — 2026-09-02

- `silentRestartToPocketNearbySync()` writes the current framebuffer row by row
  to `/.crosspoint/pocket-screen-preview.bmp` before drawing the loading popup.
  The path is transient and is removed when Nearby Sync returns to Pocket Daily.
- Only the low-memory `POCKET_SYNC` web profile advertises and serves that file:
  `/api/status` reports `screenPreviewAvailable`/`screenPreviewBytes`, and
  `/api/pocket/v1/screen-preview` streams the BMP in bounded chunks (4 KiB
  transient batch, 1 KiB stack fallback).
  Ordinary File Transfer does not expose the cached screen.
- The implementation adds no framebuffer-sized heap allocation. The 2026-09-02
  default build, strict cppcheck, and all 124 host tests passed; physical X3
  framebuffer capture and local HTTP transfer still require device sign-off.

## Resumable batched upload stream — 2026-09-02

- `src/pocket_daily/upload_stream_protocol.{h,cpp}` owns the `POCKET-PUT/1`
  grammar (header parsing, staging-path rules, CRC32, reply formatting) without
  Arduino types; `test/pocket_daily_upload_stream` covers it on the host.
- The port-82 listener batches socket bytes into a transient 4 KiB buffer
  (static 768-byte fallback), flushes sector-aligned multi-block writes,
  suspends the loop watchdog once per flush, and preallocates the staging file
  via the new `HalFile::preAllocate` wrapper when a contiguous span exists.
- Transport failures during payload retain the staging file plus an in-RAM
  verified prefix; `Resume: 1` is answered with `RESUME <received>` and
  `/api/status` advertises `uploadStreamResume`. Stale `.pocket-*.part` files
  in the destination directory are swept when a new transfer starts.
- Verified on 2026-09-02: default build, strict cppcheck, and 131 host tests.
  Throughput, watchdog margin, and resume after a real hotspot drop still need
  X3 and X4 hardware sign-off; no measured KB/s figure is recorded yet.

## Hardware test path and private-AP heap — 2026-09-03

- Hardware iteration goes through File Transfer → Join a Network and
  `scripts/pocket_put.py` (push over port 82, verified commit, verbatim reader
  replies). The user explicitly prefers this over SD-card swapping. The X3
  STA profile has no mDNS; find the reader by probing `/api/status` across the
  LAN.
- Measured 2026-09-03 on an X3 running the 2026-09-02 01:30 build: private
  Nearby Sync AP left 6.4-7.0 KB free heap; STA File Transfer left 16.3 KB. The
  retained crash report was a task-watchdog reset with breadcrumb
  `nearby:screen-preview`, and the reader hung twice more within seconds of
  the companion's post-connect preview/crash fetches. Baseline stream
  throughput on that build over STA: 6,012,768 bytes in 57.5 s (~105 KB/s).
- Mitigations in the tree: the upload stream and diagnostic handlers borrow
  the flasher's idle 4 KiB static staging buffer instead of allocating;
  `/api/status` reports `diagnosticsAffordable` and hides preview/crash
  availability below 10 KB free heap; those handlers answer 503 there.
  Physical confirmation of the new build is still pending.

## Transfer path decision — 2026-09-05

- The private Nearby Sync hotspot is the structurally weakest transport on the
  no-PSRAM X3: softAP+DHCP consume ~53 KB, leaving ~20 KB after the 2026-09
  memory work (was ~7 KB), and it is sensitive to weak signal. Every crash and
  disconnect observed in this effort occurred only on that path.
- File Transfer → Join a Network (STA) is the reliable primary path for
  firmware OTA staging and content transfer: dozens of transfers completed,
  verified, and resumed without a single failure at 100-240 KB/s. The
  companion app discovers the reader on the LAN automatically and uses the same
  port-82 stream, so no protocol change was needed.
- Decision: treat Join a Network as the default OTA/content path in guidance
  and app messaging; keep the hotspot as the fallback for readers without a
  shared Wi-Fi network. The SD startup trail (`/nearby_ap_log.txt`) proved the
  hotspot web server now starts cleanly (20.5 KB free, watchdog armed).

## Resume on another machine — 2026-09-05

Clone both repositories as siblings (`pocket-daily-firmware` next to
`pocket-daily`); the app guide and skills reference the firmware by that path.

1. Build: `./scripts/pio.sh run -e default` (stages `firmware/update.bin`,
   ignored; read `firmware/LATEST_BUILD.txt`). Host tests:
   `cmake -S test -B build/host-tests && cmake --build build/host-tests &&
   ctest --test-dir build/host-tests`.
2. Deploy for iteration: reader → File Transfer → Join a Network, find it by
   probing `/api/status` across the LAN (X3 STA has no mDNS), then
   `python3 scripts/pocket_put.py firmware/update.bin --target update.bin --host <ip>`.
   Install on the reader (Settings → System → Update firmware) and confirm the
   `-w<fingerprint>` in `/api/status` matches
   `strings firmware/update.bin | grep 'Starting CrossPoint version'`.
3. Verified on X3 with the committed tree: batched/resumable port-82 stream,
   verified commit, private-AP web server starting with ~20 KB free
   (`/nearby_ap_log.txt` trail), and an app-driven LAN firmware transfer that
   the reader then installed and reported running.
4. Open items: X4 hardware sign-off, the private hotspot path is a fallback
   (weak-signal sensitive), and `platformio.local.ini` is machine-local.

## Reader freeze and abort on a rebuilt partial section — 2026-09-08

Symptom on an X3 running `1.4.1-dev-main-551001f5`: a multi-chapter novel
stopped responding to forward page turns at `~63/125`, and a long-press Confirm
then rebooted the device. The retained crash report showed `Reset reason: panic`
/ `abort() was called`, with the last log lines laying out pages 58-63 followed
by a 4,005 ms tiled grayscale render.

Cause, in the v130 incremental-build path:

- `Section::startBuild` pins `pageCount` to a loaded partial's watermark until
  the rebuild lays out *more* pages than the partial covers. The reader's
  background pump in `EpubReaderActivity::loop()` only ticked while
  `pageCount < currentPage + BUILD_WINDOW_AHEAD`, which is false during that
  catch-up phase, so the rebuild never advanced in the background. Reaching the
  watermark then forced `render()` to re-lay the whole 63-page prefix
  synchronously while holding the RenderLock — seconds of no response to the
  page-turn button.
- That re-parse keeps expat, the CSS parser, the page LUT, and the live
  `ParsedText`/`Page` alive across the reading path's own peak (tiled grayscale
  render + font prewarm). Layout allocated pages and lines with bare `new` /
  `std::make_shared`, which with `-fno-exceptions` calls `abort()` instead of
  returning null. That is the reboot.
- The default `longPressMenuFunction` is `LP_MENU_BILINGUAL_TOGGLE`, so an
  ordinary long press ran `cycleBilingualMode()`. It dropped the section
  whenever the *committed* cache was not tagged mode-agnostic — which a partial
  never was — forcing yet another full re-parse of a monolingual chapter at the
  worst moment.

Fixes (verified: `pio run -e default`, strict `pio check`, 131/131 host tests):

- `Section::isCatchingUp()` / `mayHaveMorePages()`. The background pump keeps
  ticking through the catch-up phase, so the rebuild passes the watermark before
  the reader arrives. `mayHaveMorePages()` reads the partial's watermark trailer
  so a forward turn at a suspended build's last page still moves to the next
  spine instead of stalling.
- Heap floor `BUILD_MIN_FREE_BLOCK` (12 KB largest free block): the reader
  suspends the build — persisting it as a partial and freeing the parser —
  before a render can hit an exhausted heap. Guarded on `pageCount > 0` so a
  suspend never drops the section below the page being read.
- Every page/line/block allocation in `ChapterHtmlSlimParser` is now
  `new (std::nothrow)` with a null check that sets `outOfMemory_`.
  `parseStep()`/`finishParse()` turn that into a hard parse error and
  `Section::finalizeBuild()` abandons rather than committing a cache that
  silently drops text.
- A parse that has not met a bilingual marker is tagged `BILINGUAL_MODE_ANY`
  even when suspended as a partial, and `cycleBilingualMode()` consults the live
  parse via `currentlyModeAgnostic()`. Pages before the first marker are
  layout-identical in every mode and the rebuild re-derives them, so this is
  safe without a `SECTION_FILE_VERSION` bump: the binary layout is unchanged and
  both firmware directions read the field correctly.

Not yet verified on hardware: the reader was not reflashed with this build, so
the freeze and the abort are fixed in code review and host tests only.

## Maintenance

Record only durable decisions, verified baselines, protocol contracts, and
release evidence. Date mutable facts, name their source of truth, and replace
stale notes rather than accumulating contradictions. A memory entry must be
committed together with the change it describes; uncommitted work is not a
verified baseline.

## Live studio direction — 2026-09-19

- Agreed direction with the companion app: a live studio over the reader —
  WS event push on STA (>= 40 KB free heap), throttled `screen-live` frame
  capture under `RenderLock` (pure `LiveFramePolicy`, >= 10 KB heap gate),
  and a validated `.uipack` format (ThemeMetrics field-id whitelist,
  string/font overrides, hash-verified, atomic install, apply via
  `UITheme::reload()` metrics-source swap). Reading path stays native.
- Design docs committed, nothing implemented yet:
  `docs/live-studio-v1.md` (this repo, the contract) and
  `docs/LIVE_STUDIO_DESIGN.md` in the sibling app repo. Firmware phases
  LS-1 (WS + events) → LS-2 (screen-live) → LS-3 (pack loader) → LS-4
  (host renderer C ABI + golden parity). Flash contingency: builtin fonts
  (2.43 MB, `OMIT_FONTS` hook exists) move to SD if the engine needs room.
- The host renderer is also built here and consumed by the app as
  `libpdui_host.a` with provenance — an approved exception to the
  no-binaries-cross boundary rule for this one artifact.

## LS-2 live frames — 2026-09-19 hardware session

- Protocol path PROVEN on X3 over STA: `dev/render` trigger → render task
  capture (BMP 53,918 B = 792x528 1-bit) → `frame {seq,bytes}` event →
  chunked `screen-live` fetch returns valid BMP. hello/subscribe/status
  push flows all pass. Multi-chunk fetch gate lowered to 6 KiB
  (`LIVE_FETCH_MIN_FREE_HEAP`, commit `a5178a01`) after the 10 KiB
  diagnostics floor starved fetches at ~10.5 KiB idle heap.
- OPEN DEFECT (reproduced twice): a remote render+capture cycle wedges the
  activity loop for ~50 s or longer — render triggers time out, status
  pushes stop, the frame event arrives ~56 s late, fetches stall partial
  (8-12 KiB), and the reader stays unreachable afterwards until power-cycled.
  Suspects: SD write from the render task contending with the display/SPI
  path, a blocked `sendTXT` to a half-dead WS peer inside `handleClient`, or
  transient heap exhaustion. Needs a USB-serial session with breadcrumbs;
  do NOT trust "capture works" for studio use until this is fixed.
- Dev loop additions shipped and exercised: `POST /api/pocket/v1/dev/flash`
  (validate + flash + boot marker), `POST /api/pocket/v1/dev/render`, and
  the File Transfer boot return (commit `1d5b6940`/`469d8d4e`). The flash
  loop was used end-to-end twice; it needs exactly one Confirm after reboot.
- Also open: no WS wake-lock (the reader can sleep mid-session), and the
  deployed dev build's version string lags its commit (`-w<worktree>` from
  pre-commit builds) — build after committing for exact install checks.
- The File Transfer status screen only repaints on Wi-Fi-bar changes;
  arrow keys do nothing there. Use `dev/render` to force a render.

## LS-1 hardware sign-off — 2026-09-19

- X3 (`5B09AF70`) installed `1.4.1-dev-main-df75f78d` and joined STA File
  Transfer: `/api/status` advertised `liveStudio {mode:"push", wsPort:81}`
  with ~14 KB free. A raw WebSocket client verified hello (proto live-studio/1
  + deviceID), ping→pong, subscribe→immediate status snapshot, and repeated
  status pushes; unsubscribe + close were clean. `df75f78d` also lowered the
  listener gate to 12 KB after the draft 40 KB gate never opened (measured
  ~15 KB free in this profile) and suspended pushes during active uploads.
- Post-install boot returns to the Pocket Daily shell with Wi-Fi off —
  intended: the radio is powered only inside network activities, so push
  exists only while the reader sits in a network mode.
- Not exercisable on STA: the `prefs` event. `/api/pocket/v1/preferences` is
  registered for POCKET_SYNC only, so a FILE_TRANSFER client gets 404.
  Extending preferences (or a studio read path) to FILE_TRANSFER is a
  candidate LS-2-adjacent change; the event itself is covered by host tests.

## LS-1 live-studio event push — 2026-09-19

- `src/pocket_daily/live_studio/LiveStudioEvents.{h,cpp}` implements the
  pure event protocol (hello/status/prefs/bye encoders, subscribe/unsubscribe/
  ping parser with interval clamping, `shouldCaptureFrame` policy) and is
  host-tested in `test/live_studio/`.
- `CrossPointWebServer` starts the WebSocket on STA when free heap ≥ 40 KB
  (`liveStudioPush`), sends `hello` on connect, pushes the full status JSON
  on stable-field change (15 s keepalive, 500 ms spacing), broadcasts
  `prefs` after a preferences POST, and `bye` on stop. Legacy binary/START
  upload frames are now FULL-profile only. `/api/status` advertises
  `liveStudio {mode, wsPort, frameStream:false, uiPacks:false}`.
- Verified: default build, strict cppcheck, 147/147 host tests. Physical X3
  push behavior (subscribe + status push while reading) is pending hardware
  sign-off; the app-side consumer landed as M1 in `pocket-daily`.
- Toolchain note: the registry `tool-cppcheck` package for darwin_arm64
  ships an Intel-only binary (`Bad CPU type`). The native 2.11 build now
  persists at ignored `build/native-cppcheck` and
  `pio check -c build/native-check.ini` points there — temp-dir overrides
  get wiped between sessions.
- Format note: the host `bin/clang-format-fix` (v23) wraps long string-literal
  calls and include blocks differently from CI's LLVM-21 clang-format. For
  new files with long literals, expect CI to reject the local output; apply
  CI's diff or update the local pinned formatter before pushing.

## Multi-agent collaboration — 2026-09-19

- OpenCode, Claude Code, and Codex all work in this repository and in the
  sibling app repository. Each repository's `AGENTS.md` is the single
  operational entry point for every agent, `CLAUDE.md` holds the shared
  constraints, and this file is the shared cross-agent memory. No agent keeps
  a private instruction file or separate memory.
- Cross-repository protocol work (Nearby Sync v1, upload stream, direct
  sessions) proceeds on both sides in one coordinated effort: verify the
  companion implementation whenever an endpoint, record, or file layout
  changes, and record the paired commits here.
- On 2026-09-19 the previously uncommitted working tree (reader partial-build
  fixes, companion transport sessions, OPDS panic repair) was re-verified
  (default build, strict cppcheck 2.11, 140/140 host tests) and committed as
  `5f1806a3`, `3ddfe641`, and `3d690840`; the app-side counterpart landed in
  `pocket-daily` as direct reader sessions.

## Companion transport sessions — 2026-09-09

- Pocket Sync offers Join a Network (saved reader Wi-Fi) and Nearby Sync
  (existing BLE/private AP). The original STA server profile is preserved.
- `/api/status` adds optional `deviceID` matching BLE and `sessionEnd` for the
  private AP only. POST `/api/pocket/v1/session/end` rejects active transfers,
  responds, then returns to Pocket Daily through the existing low-memory restart.
  Status heartbeats no longer keep an idle AP alive; no automatic flashing occurs.
- The app queues local copies before AP handoff, keys retry UUIDs and installation
  checks by device identity, and defers automatic diagnostics on the private AP.
  Resume state remains RAM-only; reader restart falls back to RESUME 0.
- Default firmware build and 132 host tests passed. Physical iPhone/X3 no-router
  transfer, heap/watchdog, interruption/retry, and install confirmation remain
  pending; see the companion's `docs/CONNECTIVITY_VALIDATION.md`.

- Strict cppcheck passed using a native build of the pinned 2.11 upstream release
  through ignored build/native-check.ini; the registry mirror was unavailable.

## X3 installation verified — 2026-09-09

- After user-side installation, live STA /api/status reported
  `1.4.1-dev-main-fa92806c-wf9376b5a`, matching the staged image exactly, with
  a fresh software restart. New deviceID and sessionEnd=false fields were
  present; port 82 and resume remained available. Direct AP and updated iPhone
  app verification are still pending.

## OPDS catalog allocation panic — 2026-09-11

- User-reported crash report from installed `1.4.1-dev-main-fa92806c-wf9376b5a`
  showed `panic`/`abort`. Symbolization against the preserved matching ELF reached
  `OpdsParser::endElement` vector reallocation/copy while parsing an HTTP feed;
  HTTP-open log showed 9,420 B free / 6,900 B largest block. This report does not
  implicate the user's PDF transfer. Raw report and ELF remain in ignored build/.
- OPDS fetch now streams at most 1 MiB to `/.crosspoint/opds-feed.tmp`, closes
  HTTP and file handles, then parses in 128-byte reads. Scratch is removed on
  completion/failure. Old catalog capacity is released before fetching.
- Parser bounds fields to 2,048 bytes and catalogs to 128 entries, moves entry
  strings, checks contiguous/free heap before growth (4 KiB headroom), and stops
  with an error rather than claiming success with a truncated catalog. Pagination
  capacity is reserved before transferring ownership to the browser.
- Scope is crash repair first; PDF rendering remains unsupported. Host tests use
  bundled Expat with firmware XML flags. Hardware OPDS retry/sign-off is pending.
- Verification: default build, strict cppcheck 2.11 and 140/140 host tests passed.
  Staged `1.4.1-dev-main-fa92806c-w8ea71718` to the X3 over STA: first stream
  interrupted; resume acknowledged offset 1,201,932; final OK/commit 200 matched
  6,023,216 bytes and CRC32 DDC8E685, publishing `/update.bin`. SHA-256:
  `38795190893c922299a92422e3c9170a02827fc5429d2e00fc4558713dbb6635`.
  This proves staging, not installation or a hardware crash fix.
