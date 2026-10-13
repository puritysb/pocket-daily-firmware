# Pocket Daily Firmware Project Memory

This is concise, repository-owned context for future sessions. It is not a chat
transcript. Current source and release records override dated observations.
Read it for repository boundaries, contracts and durable state; read the cited
`docs/*.md` for detail. The full dated history up to 2026-10-13 is archived in
`docs/history/PROJECT_MEMORY-to-2026-10-13.md` (historical, may contradict code).

## Repositories and collaboration

- Firmware: this repository (`puritysb/pocket-daily-firmware`). App: sibling
  `pocket-daily` next to this clone (`puritysb/pocket-daily`). Resolve the
  sibling relative to this checkout; absolute `~/github/` paths are stale.
- Firmware owns device behavior, endpoints, local persistence, memory gates and
  flashing. The app owns the Apple-platform client. Every contract below spans
  both repositories: change and verify both sides together and record paired
  commits here.
- Remote topology: `origin` = this repo, product branch `main`; `upstream` =
  `crosspoint-reader/crosspoint-reader`, foundation `upstream/master`. Product
  directories are downstream-only; merge upstream, never rebase product history.
- OpenCode, Claude Code and Codex share `AGENTS.md` (only instruction file) and
  this memory. Scoped guides are `lib/Epub/AGENTS.md` and other nested
  `AGENTS.md`; skills live in `.claude/skills/` and are indexed from `AGENTS.md`.
  Do not reintroduce `CLAUDE.md` (it would hide `AGENTS.md` from Claude Code).
- Upstream sync hazard (#3058, 2026-08-15): upstream made `AGENTS.md` real,
  `CLAUDE.md` a symlink, and moved skills to `.skills/`. On merge keep the fork's
  `AGENTS.md`, port rule changes into it or `docs/embedded-reference.md`, keep
  `CLAUDE.md` deleted and skills under `.claude/skills/`.
- The old combined `crosspoint-agentdeck` repository mixed firmware, AgentDeck
  and app memories; promote only verified firmware-relevant facts from it.

## Upstream base and fork delta

- Base: CrossPoint 1.6.5 (`93e98bb7`) integrated on `main` at `ee188fe7`
  (PR #4), FreeInk SDK `111fdcc7`. wolfSSL intentionally excluded.
  `docs/upstream-integration-1.6.5.md`.
- `docs/SEAM.md` is the source of truth for the Pocket/inherited boundary: hook
  inventory, exceptions, carried-patch register, include rules and the mirror
  obligations (status base fields, silent-reboot magic `0xC1EAB007`,
  reboot-target numbering, hidden-file rule, radio watch). Register any new
  Pocket touchpoint in an inherited file in the same change.
- `docs/fork-delta-register.md` classifies every modified inherited file
  (upstream candidate, device patch, product hook, duplicate, regression). A
  candidate is not submission-ready because code or a benchmark exists here.
- Upstream PR rules live in the `fork-sync` skill. Lessons from #3813/#3814
  (2026-10-01): never argue heap safety without an allocation measurement,
  state "not verified on hardware" when true, and keep the AI-usage line
  accurate. Status 2026-10-05: #3813 open; #3814 Draft at `236cde79`
  (32-entry `int16_t` gap window); the reviewer's X3 run covered the earlier
  64-entry revision, so the final revision still needs device acceptance.
- Owner decisions 2026-10-01: Pocket Daily's first button reads Home and the
  Home entry is named Pocket Daily; a book returns to the shell that opened it
  (`goToReader()` resets origin to Home, `resumeReader()` keeps it); stock
  Home/Library/File Browser host no BLE window; sleep default/Blank screens are
  stock; themes are stock except per-label `UiCjkFont`, `StatusBarTitle` and
  Home paging. Home menu is stock plus one entry; Articles opens from Pocket
  Daily Home's Left button.
- Removed features (do not resurrect without a product decision): AgentDeck
  daemon and its settings/OTA paths (2026-09-25), games (2026-09-24), UI packs,
  live frame capture and saved screen preview (2026-10-01). The WebSocket
  status/prefs push and `liveStudio.mode`/`wsPort` remain.

## Versions and releases

- Product version 0.1.0, `firmwareLineage` 2 (lineage 1 = `1.0.0-beta.*` and
  `1.0.0-dev-*`). `/api/status` reports lineage and CrossPoint base. Contract:
  `docs/product-versioning.md`.
- 0.x releases are published as normal (latest) GitHub releases tagged
  `pocket-v<version>`; `1.0.0` is the first stabilized release. Reason:
  `/releases/latest` must not resolve to historical `v1.6.6` while every product
  release is a prerelease. As of 2026-10-13 `pocket-v0.1.0` is not tagged;
  newest tags are prereleases `pocket-v1.0.0-beta.1` and `v1.7.0-beta.4`.
  Tagging needs the owner's direct confirmation and exact-image device pass.
- GitHub releases are the production OTA source; the asset name must be exactly
  `firmware.bin`. Stable release requires exact-artifact X3/X4 sign-off in
  `docs/release-checklist.md`. `firmware/LATEST_BUILD.txt` names the locally
  staged (ignored) artifact; take artifact identity from it.
- Dev images report `<version>-dev-<branch>-<sha>[-<env>]-w<fingerprint>`.
  Build after committing so the version matches the commit; verify an install
  by exact version on the same device identity.
- App Store timing is independent, but Nearby Sync v1 must be frozen and
  compatible before either side is presented as production-ready.

## Hardware constraints

- X3 and X4 are no-PSRAM ESP32-C3 readers. X3 panel 528x792. Host builds and
  tests never satisfy hardware sign-off.
- X3 GPIO13 is SD power in `BoardConfig`; on X4 it is the battery latch. Do not
  generalize the HAL comment into a claim that the X3 CPU loses power.
- X3 STA has no mDNS: find it by probing `/api/status` across the /24.
- Private AP (softAP + DHCP) costs ~53 KB and is weak-signal sensitive; every
  crash/disconnect in the 2026-09 transport work occurred on it. STA (Same
  Wi-Fi) is the primary path (100-240 KB/s historically); AP is a fallback.
- X3 cannot run its own HTTPS update check: TLS needs ~45 KiB with symmetric
  16 KiB records (`MBEDTLS_ERR_SSL_ALLOC_FAILED` at ~31 KiB free). Fix would need
  `custom_sdkconfig` for the framework; deferred. The app's Update reader is the
  primary route; the reader shows the memory reason and the app hint.
- Installed Arduino libs already disable Wi-Fi TX/RX IRAM optimization; do not
  credit SDK tuning with blanket heap gains. The extra 8 KiB main stack has a
  documented overflow rationale; do not shrink it blindly.

## Memory gates

- `docs/nearby-sync-v1.md` defines the mandatory runtime heap and
  responsiveness gates for both models. Do not lower an unmeasured safety budget
  to make something work; investigate resource lifetime first.
- Dedicated Sync profiles COMPANION (Same Wi-Fi) and POCKET_SYNC (BLE/private
  AP) are poll-only: no listener startup, optional SD diagnostics skipped.
  Content presentation requires 16 KiB free and a 4 KiB largest block.
- Sync purges this server's TIME_WAIT PCBs on ports 80/82 (`ServerTimeWait.*`);
  without it repeated status reads leaked ~256 B each for 120 s
  (`docs/sync-route-memory.md`). Dedicated Sync dispatches exact method/path
  pairs through one fallback instead of per-route Arduino handlers.
- BLE windows: 96/40 KiB start, 24/8 KiB ready/admission, 20/4 KiB running
  (free/largest block). Automatic admission may release rebuildable font caches
  under the render lock and re-sample; thresholds unchanged.
- Reader build: `BUILD_MIN_FREE_BLOCK` 12 KB suspends a section build to a
  partial; layout allocations are `new (std::nothrow)` and OOM abandons the
  build rather than committing a truncated cache. Word chunks are 192 words.
- `/api/status` `diagnosticsAffordable` hides preview/crash availability below
  10 KB free; those handlers answer 503 there.

## App–reader contracts (all v1 unless noted)

- Nearby Sync and preferences: `docs/nearby-sync-v1.md`. Preferences GET/POST
  validate the whole body and roll back every field on a failed save; absent keys
  mean unsupported and the app sends only keys GET reported. Fields include
  `sideButtonLayout` (0..2), `frontButtonFollowOrientation`, `sleepWakeIndicator`,
  orientation 0..3, line spacing 0..3, margins 5..40.
- Status: `/api/status` carries `deviceID` matching BLE, lineage/base, optional
  `totalHeap`, capability flags below, `readerPerf` and `lastBuildError`
  (`docs/reader-perf.md`, `docs/build-failure-log.md`).
- Upload stream `POCKET-PUT/1` on port 82: `uploadStreamWindow:4096` with ACK
  per SD-accepted block, `Resume: 1` → `RESUME <n>`; resume state is RAM-only.
  `publicationReceipt:1` is a bounded latest-only SD receipt with idempotent
  commit; a missing receipt stays unknown. Not general SD atomicity.
- `transferControl:1` (`docs/transfer-control-v1.md`), `readerFiles:1`
  (`docs/reader-files.md`), `readingProgress:1` (`docs/reading-progress-v1.md`,
  app `docs/READING_PROGRESS.md`), BLE reading sync READ1
  (`docs/reading-sync-ble-v1.md`), `pocketProfile:1`
  (`docs/pocket-profile-v1.md`), `pocketGlance:1` (`docs/pocket-glance-v1.md`),
  screen presentation (`docs/pocket-screen-present-v1.md`), articles
  (`docs/articles-v1.md`), live status push (`docs/live-studio-v1.md`).
- Reading lists advertise `offerVersion:2`; optional `readerSeq` binds an offer
  to the observed record. PDRO writes v2 and reads v1/v2; PDRP is v1.
- Positions are crengine-style XPointers. Section page records carry the text
  offset of their first character, so exits record exact positions and accepted
  offers map back exactly. Shared fixture
  `test/reading_progress/fixtures/firmware-xpointers.json` must resolve to the
  same text in the app's `testFirmwareXPointersResolveToTheSameText`.
- App content cards: PDCM manifest, PDCT cards (v2 adds ImageFirst/SideBySide),
  canonical PBM images, prepare/state/activate/read endpoints
  (`docs/content-*.md`, `docs/content-card-layout-v2.md`). Published revisions
  are immutable; activation uses two CRC/generation slots.
- Host renderer: `host/` builds `PocketUIHost` (C ABI) and an Apple XCFramework
  with provenance JSON; the app imports it under the approved binary exception
  (app `docs/HOST_RENDERER.md`). Re-import after renderer changes.
- Developer-update return: the origin marker routes a reboot back to Sync or the
  matching chooser (`docs/dev-update-return.md`).

## BLE standby and app wake (X3, developer `ble_standby` env)

- `scripts/pio_ble_standby.sh` builds env `ble_standby` (extends `default`, adds
  PM/tickless/BT modem sleep in an isolated SDK copy). It is SDK automatic light
  sleep, not BLE wake from deep sleep. `default`/`gh_release` keep bounded BLE
  windows; X4 standby is gated off. Review: `docs/ble-standby-review.md`.
- After the sleep frame is saved and the framebuffer released, a bonded X3
  advertises `WAKE1` and accepts authenticated `START_WIFI <id>`, then restarts
  once into saved Wi-Fi. Buttons route to reading. Falls back to deep sleep for
  disabled sync, no bond, failed memory admission or low battery (>5% with
  confirmed charging, otherwise >10%); exit reason `low-battery` is distinct.
- App Connect checks LAN first, requests BLE wake only when needed, validates
  identity on LAN, and suspends periodic BLE/LAN exchange during idle tests.
- Fresh X3 without settings offers authenticated BLE setup once, with Read
  offline and a five-minute idle exit. Never recommend Pocket Daily via
  CrossPoint's unlocker on locked units (see app `docs/READER_FIRST_USE.md`).
- Wake cue replaces the old refresh with no animation dwell; an active panel
  waveform cannot be cancelled; input during BUSY is not established.

## Reader engine facts

- Section cache v134 (`lib/Epub/Epub/Section.cpp`); cache formats, version
  bytes and invalidation rules are in `lib/Epub/AGENTS.md`. Partials carry a
  format tag; a parse before any bilingual marker is tagged mode-agnostic.
- `ReaderLayoutAhead` lays out ahead on the render task after each page and
  yields to buttons; next chapter pre-builds while idle on the last page.
- Advance cache is a bounded LRU with batched reads; the page glyph set is
  pinned while drawing. Uncovered glyphs fall back to
  `/.fonts/PocketSymbols/PocketSymbols_12.cpfont` (`docs/sd-card-fonts.md`).
- `SdCardFont::BoundedUI` (`docs/bounded-ui-fonts.md`) is opt-in for UI text.
- Directory iteration must check the still-open parent's error state at EOF
  (SdFat clears the child); fixed 2026-10-09 with host regressions.
- OPDS streams ≤1 MiB to a temp file then parses with bounded fields/entries.

## Developer pipeline, flashing and recovery

- `scripts/dev_pipeline.py` (`docs/developer-pipeline.md`) enrolls one Same-Wi-Fi
  developer reader in ignored local config, runs gates (`--checks` = full host
  suite, every Python test, strict analysis, builds), freezes the candidate,
  optionally installs with `--flash`, and drives the sibling app's real Debug
  model through shared opt-in XCTest scenarios. Evidence stays in ignored
  `build/dev-pipeline-runs/<run>/report.json`. Pass `--app` explicitly.
- Stale addresses are recovered through the app's normal discovery with an
  enrolled-identity gate. Close normal companion instances before a run.
- Owner authorization covers automatic developer flashing inside this pipeline.
  Developer routes (`dev/flash`, `dev/ble-cycle`, `dev/network-fault`, capture,
  reset) exist only with `ENABLE_DEV_REMOTE_FLASH`/dev flags; `gh_release`
  strings must contain none of them. Shipping installation always needs the
  reader's Confirm; staging is not installation.
- Developer reset archives settings, Wi-Fi and bonds and preserves book bytes;
  it is excluded from shipping builds and requires identity plus
  acknowledgement. Archives exist; do not reset blindly.
- Owner preferences: minimize SD card swaps, no speculative connection-mode or
  radio churn, keep the shared-network setup, no blind repeated uploads. If the
  reader goes offline, read its screen first (network menu = Wi-Fi abandon,
  Pocket shell = reboot, dark = power).
- Manual path: `./scripts/pio.sh run -e default` stages `firmware/update.bin`;
  `python3 scripts/pocket_put.py firmware/update.bin --target update.bin --host
  <ip>` with `--dev-flash` for developer images. `platformio.local.ini` is
  machine-local.

## Current verified baseline — 2026-10-14

- `main` `14bf9624` (code as `95be13f6`); app counterpart `005ec63` plus its
  2026-10-14 working tree. Installed X3 image
  `0.1.0-dev-main-14bf9624-ble-standby-w8e68d8a9` (matches `LATEST_BUILD.txt`).
- 2026-10-14 on that image: BLE-only reading acceptance `20261013T170240-a41674`
  (`--continuity-suite --ble-continuity`; 8 standby legs incl. unarmed control,
  window min heap ~62.7 KB) and the full Mac/X3 regression `20261013T171736-315400`
  (10 interface cases, two Wi-Fi outages, 14 captures, real-EPUB HTTP continuity,
  BLE wake).
- Gates: 935 host tests, 135 Python tests, strict analysis, default and
  standby builds. Mac/X3 aggregate `20261013T131302-494933` passed 139 Mac
  regressions and physical run `20261013T131358-21d2ed` (BLE failed-network
  rollback, 10 interface cases, bidirectional reading rounds, screen/sleep/wake
  captures, original-state restoration). Reset can remove `/.pocket`; capture
  ticket writes now recreate it.
- Fresh-settings BLE onboarding (pairing code, keychain autofill, Wi-Fi
  transfer, LAN discovery, Welcome EPUB) passed on Mac/X3 on 2026-10-13.
- Real EPUB continuity `20261012T153747-86b9c6`: byte-identical
  import/transfer/download, two real Wi-Fi interruptions, timer deep sleep.
- Wi-Fi loss during upload/after publication `20261011T173129-98ca79` and delayed
  advertising recovery `20261011T152735-7c55e9` (3/3) passed.
- Standby: three 90 s cycles `20261011T122403-b8cecb` (reconnect 13-15 s,
  BLE min heap ~64.4 KB, largest block 55,284 B); one 30-minute idle
  `20261011T105840-d292e9` (reconnect 12.2 s).

## Open limitations

- Not established: X4 acceptance of nearly everything above, physical iPhone
  flows, measured current draw, overnight or battery-only standby, long-term
  leak absence, optical panel quality, input during the panel waveform,
  image-rich EPUBs, BLE-only exchange outside developer standby (book close,
  wake, pre-sleep windows), remote-frame playback.
- Unexplained: the historical app download timeout and the historical RF
  advertising delay; router reboot/DHCP changes are untested.
- Storage: aggregate SD quota, free-space admission/reservation and durable
  orphan/staging cleanup remain open (`docs/content-storage-admission.md`).
  Full-FAT free-cluster scans are unbounded; never put one on a radio path.
- Reader HTTPS OTA on X3 is memory-blocked (see Hardware constraints).

## Toolchain and host notes

- Host tests: `cmake -S test -B build/host-tests && cmake --build
  build/host-tests && ctest --test-dir build/host-tests`.
- Strict analysis uses cppcheck 2.11. CI builds it natively (upstream commit
  `f156b1ec`) because espressif32 55.03.311 reinstalls an incompatible
  tool-cppcheck; locally `build/native-cppcheck` with
  `pio check -c build/native-check.ini`.
- Local `bin/clang-format-fix` (LLVM 23) wraps long literals differently from
  CI's LLVM 21; expect CI diffs for new files with long literals.
- macOS Local Network privacy can block Homebrew and `/usr/bin/python3` LAN
  access while `/usr/bin/curl` works; an empty Python scan is not evidence that
  the reader is absent.

## Maintenance rules

- Add only durable decisions, verified baselines, protocol contracts or
  non-obvious constraints; date mutable facts and name their source of truth.
- Replace stale notes instead of accumulating contradictions. Keep this file
  under ~300 lines; move superseded detail to `docs/history/` instead of
  appending. Per-run narratives belong in run reports or the cited docs.
- A memory entry must be committed together with the change it describes.
  Uncommitted work is not a verified baseline.

Archive: `docs/history/PROJECT_MEMORY-to-2026-10-13.md`.
