# Pocket Daily Firmware Project Memory

This is concise, repository-owned context for future sessions. It is not a chat
transcript. Current source and release records override dated observations.

## Repository split

- Firmware: `/Users/puritysb/github/pocket-daily-firmware`
  (`https://github.com/puritysb/pocket-daily-firmware`)
- App Store app: `/Users/puritysb/github/pocket-daily`
  (`https://github.com/puritysb/pocket-daily`)

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

## Maintenance

Record only durable decisions, verified baselines, protocol contracts, and
release evidence. Date mutable facts, name their source of truth, and replace
stale notes rather than accumulating contradictions.
