# Pocket Daily Firmware Project Memory

Current source/release records override dated observations. Details: cited docs.
Pre-2026-10-13 archive: `docs/history/PROJECT_MEMORY-to-2026-10-13.md`.
2026-11-04: owner-authorized OPDS origin safety repair; GET Basic/custom headers bind to configured origin, HTTPS downgrade rejected. Host1194/default+gh_release warning0/strict cppcheck/app image validator pass. Frozen local1.0.0 SHA d2aeaed3…e606f53e, build/release-security-20261104/. Not flashed or hardware-accepted; docs/http-credential-origin.md. Existing storage SDK patch preserved.

2026-11-05 owner-install: original slots/known SD state preserved in
`build/owner-install-20261105/`. Owner resumed all three devices after the earlier
X4-only pause. X3 fresh reading/raw-state backup, same-path Sync resume (offset0),
atomic commit and entire /update.bin readback now match frozen candidate SHA.
Native X3 confirmation remains pending. X4 owner reported install/reconnect done,
but fresh identity-bound status remains developer wdd553a3b; owner did not observe
progress and requested retry. Both native confirmations remain required; no remote
flash/trial/acceptance. Production --flash guards retained; no USB reader enumerated.
App handoff owns iPhone TestFlight and full setup status.

## Sleep navigation fix in development (2026-11-01)

- Immediate reader installation after wake bypassed the pending-only origin assignment, so Back went Home. Origin is now set before installation; reader origin and parent shell persist through sleep/internal reader restart, with legacy fallback.
- Branch `codex/preserve-sleep-navigation`; final developer w7317c875 installed on X3/X4. Owner-operated Sleep/Wake/Back returned to Pocket Daily on both; state readbacks confirm context. Host 1,175/default build/strict analysis pass. Contextual footer/menu labels corrected with atomic destination reads/writes; full physical label coverage is unverified. Proof: `docs/sleep-navigation.md`, `build/sleep-navigation-hardware-20261101/`. Release/distribution held.

## Keyboard remote development (2026-11-01)

- Owner: default OFF; release/distribution stays on hold. Contract: `docs/keyboard-remote-v1.md`.
- Merged app #19 / firmware #26 into main (2026-11-01); feature branches/worktrees cleaned. Other app work and existing SDK storage patch preserved.
- X3 opt4 `1.0.0-dev-codex-keyboard-remote-a3fd9694-w319d57e0`: 24 turns passed; median ACK 1.601s (was 4.789s), 22 streamed / 2 chapter fallbacks; checkpoint min free/block 56,824/40,948B.
- X4 lacked a reader bond; owner ended further pairing trials and accepted code-level verification only. No X4 remote/power hardware sign-off.
- 4KiB text streaming retains BLE; chapter/image fallback retains radio-off rendering and unchanged memory gates. 80MHz idle/panel-off policies implemented; USB-off follow-up observed 47mA connected idle vs prior 49mA, awake OFF 23–24mA (short onboard comparison). Evidence/backups: `build/keyboard-remote-perf-20261101/`; battery follow-up `build/keyboard-remote-power-20261101/`; baseline `build/keyboard-remote-20261101/`.

## Repositories and collaboration

- Firmware: this repository (`puritysb/pocket-daily-firmware`). App: sibling
  `pocket-daily` next to this clone (`puritysb/pocket-daily`). Resolve the
  sibling relative to this checkout; absolute `~/github/` paths are stale.
- Firmware owns device behavior/storage/memory/flashing; the app owns its client.
  Change and verify contracts on both sides together; record paired commits.
- Remote topology: `origin` = this repo, product branch `main`; `upstream` =
  `crosspoint-reader/crosspoint-reader`, foundation `upstream/master`. Product
  directories are downstream-only; merge upstream, never rebase product history.
- Agents share `AGENTS.md` and this memory. Scoped guides/skills are indexed
  there; never add `CLAUDE.md`, which would hide that guide from Claude Code.
- Upstream sync hazard (#3058, 2026-08-15): upstream made `AGENTS.md` real,
  `CLAUDE.md` a symlink, and moved skills to `.skills/`. On merge keep the fork's
  `AGENTS.md`, port rule changes into it or `docs/embedded-reference.md`, keep
  `CLAUDE.md` deleted and skills under `.claude/skills/`.
- The old combined `crosspoint-agentdeck` repository mixed firmware, AgentDeck
  and app memories; promote only verified firmware-relevant facts from it.

## Upstream base and fork delta

- Base (in progress 2026-11-06, `feature/upstream-2.0.0`, uncommitted):
  CrossPoint 2.0.0 `42d58daa`, FreeInk SDK `5e94fe74`. Owner decision: 1.0.0
  ships on 2.0.0 even if late. Fork keeps its reader engine, settings and web
  server; adopts plugins (default off, `pluginsEnabled`, auto-on when the card
  has plugins), OPDS 2.0 with origin-bound credentials, device fixes. No
  wolfSSL, LCP out, bold-italic falls back to bold (flash), section v138.
  Plugin distribution layers A (firmware) / B (app CrossPoint-mode install) /
  C (Hub plugin, publish needs owner approval). `docs/upstream-integration-2.0.0.md`.
- Previous base: CrossPoint 1.6.5 (`93e98bb7`) on `main` at `ee188fe7` (PR #4);
  `docs/upstream-integration-1.6.5.md`. The 2026-10-28 develop audit and reader
  A/B pilots are archived in `docs/history/PROJECT_MEMORY-before-2.0.0-2026-11-06.md`.
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
- Owner decision 2026-10-31: stock defaults for new installs — power-on opens
  Home (Pocket Daily via `startupApp`) and Long-press Menu is Disabled. Saved
  settings keep their values. The bilingual view mode was removed the same day:
  translated books ship as one standard EPUB per language.
- Removed features (do not resurrect without a product decision): AgentDeck
  daemon and its settings/OTA paths (2026-09-25), games (2026-09-24), UI packs,
  live frame capture and saved screen preview (2026-10-01). The WebSocket
  status/prefs push and `liveStudio.mode`/`wsPort` remain.

## Versions and releases

- Owner decision 2026-11-01: the first release is 1.0.0 (0.x never published);
  only release (`1.0.0`, shown "Pocket 1.0.0") and developer builds
  (`1.0.0-dev-…`, shown "Pocket 1.0.0 dev <commit>") exist. `firmwareLineage` 2
  (lineage 1 = old `1.0.0-beta.*`, never reused). Contract: `docs/product-versioning.md`.
- Releases are normal (latest) GitHub releases tagged `pocket-v<version>`, so
  `/releases/latest` never resolves to historical `v1.6.6`. Checked 2026-10-25: no `pocket-v` release is published;
  latest is `v1.6.6`, excluded by current product update parsing. No current
  app channel offers a supported image; old betas remain historical.
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

- Connected inputs: `docs/layout-inputs-v1.md`, capability1/2, PDLI≤8KiB with
  CRC/UTF-8 validation and same-reader active cards. Schema2 adds bounded current
  book cover chunks and asleep/WAKE labels for actual Home/Brief; schema1 keeps
  Sync placeholder/footer. Paint-time header receipt≤192B, no persistent scene
  or framebuffer. Actual UI uses canonical World12 Cached/fallback independently
  of the reading font; cards/legacy Sync use BoundedUI. Native ABI1 additive.
- BoundedUI font reads retain two HAL handles and a 2 KiB metadata cache while
  loaded (total owned read state ≤2,816 B, excluding HAL internals). Bitmaps
  bypass it; I/O faults latch invalid state. Cached mode is unchanged.
  `docs/bounded-ui-fonts.md` owns measurements and limits.
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

## BLE standby and app wake (developer `ble_standby` env)

- 2026-11-03: default-OFF Home app connection passed Mac/X3/X4. Resumable panel
  restoration, release-before-repaint and bounded actual-style SD title warming
  fixed the regression; X4 physical Back7172→1910ms (owner1–2s). Latest developer
  wdd553a3b is installed on both: automatic Home X3 1006/X4 1071ms, original state
  and staging restored. Owner reports strong overall improvement; per-menu results
  remain unspecified. Keep phase-specific memory gates/shaping/pinned ownership.
  Lessons/evidence: `docs/menu-navigation-audit.md` (Lessons learned) and
  `build/home-connect-20261102/`. No extra wall-clock/battery claim; release held.

- `scripts/pio_ble_standby.sh` builds env `ble_standby` (extends `default`, adds
  PM/tickless/BT modem sleep in an isolated SDK copy). It is SDK automatic light
  sleep, not BLE wake from deep sleep. `default`/`gh_release` keep bounded BLE
  windows. The 2026-11-02 opt-in extension admits original C3 X3/X4 via HAL;
  w02b1fbaf passed Mac/X3 and Mac/X4 bonded BLE→Same Wi-Fi plus settings restoration;
  short light-sleep samples only, no energy/overnight claim. Review: `docs/ble-standby-review.md`.
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

- Section cache v137 (`lib/Epub/Epub/Section.cpp`, bilingual header byte removed); cache formats, version
  bytes and invalidation rules are in `lib/Epub/AGENTS.md`. Partials carry a
  format tag.
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

## Current 1.0.0 preparation

- The 1.6.5-base candidate (`6025da03`, `docs/release-1.0.0-candidate.md`) is
  superseded by the 2.0.0 base; its gates must be re-run on a new 2.0.0-base
  image. Archived: `docs/history/PROJECT_MEMORY-before-2.0.0-2026-11-06.md`.

## Open limitations

- Not established: physical buttons beyond these suites, iPhone
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

Follow AGENTS for memory maintenance; working-tree evidence is not a committed baseline.
