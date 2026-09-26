# The Pocket Seam

This document defines the boundary between inherited upstream code
(crosspoint-reader) and the Pocket Daily product stack, so that routine
`git merge upstream/main` stays mechanical. Every Pocket modification to an
inherited file is either a **hook** (≤10 lines at its call site), a
**registered exception** (E1–E5 below), or a **carried patch** (product
behavior in an inherited file, listed in the register). Everything else lives
under `src/pocket_daily/`.

Ownership background: `docs/product-architecture.md`. Operational context:
`docs/PROJECT_MEMORY.md`.

## Why this shape

The fork's merge-base with upstream is `2754a5ff`. Before the seam sprint the
product stack was inlined into the inherited files — `CrossPointWebServer.cpp`
alone carried +1,696 Pocket lines — so every upstream merge re-litigated the
same large conflicts. The sprint (S1–S8) moved the product stack into modules
and reduced the inherited-file footprint to the hooks below. Upstream has no
paths under `src/pocket_daily/`, so module files themselves never conflict;
conflicts can only land at hook sites and carried patches.

## Module inventory (`src/pocket_daily/web/`, `src/pocket_daily/boot/`)

| Module | Owns |
|---|---|
| `web/Profile.h` | `Profile` enum (FULL / POCKET_SYNC / POCKET_NEARBY_SYNC gating) |
| `web/UploadStreamServer.{h,cpp}` | port-82 upload data plane, `StagedUpload` ledger, `publishHttpStaged()` |
| `web/PocketStatus.{h,cpp}` | `/api/status` JSON builder + `diagnosticsAffordable()` |
| `web/LiveStudioService.{h,cpp}` | WS push listener, subscriptions, transfer focus, active-pack state |
| `web/PocketEndpoints.{h,cpp}` | every `/api/pocket/v1/*` route (registration + handlers) |
| `web/Host.h` / `RouteDeps` | plain-function-pointer host reach-in bundles |
| `web/PrivateApPolicy.{h,cpp}` | private-AP credentials, heap start gates, profile select, watchdog, `apBootLog` |
| `web/StaRadioWatch.{h,cpp}` | passive STA association tracking (`StaAction`) |
| `web/RadioHealthPolicy.h` | association grace/repaint policy (host-testable, header-only) |
| `boot/ProductBoot.{h,cpp}` | JP font install, silent restarts, NetHealth boot, startup pack, dev-boot return |
| `live_studio/UiPackStore.{h,cpp}` | `.uipack` storage incl. `listPacks()` streaming scan |

Older Pocket modules (`nearby_sync/`, `upload_stream_protocol.*`,
`direct_session.h`, `live_studio/*` evidence tooling) predate the sprint and
were already seam-shaped.

## Hook inventory

### Bounded UI font access (carried rendering patch)

`lib/EpdFont/SdCardFont.*` has an opt-in `BoundedUI` mode for a future live content
view. `EpdFontData`/`EpdFont` add nullable disk-backed kerning/ligature callbacks;
cached and built-in fonts keep their existing path. CPFONT v4 bytes are unchanged.
The live content presenter selects this mode. See `docs/bounded-ui-fonts.md` for the memory
bound, host/fixture verification and remaining integration. Preserve this mode
when merging font-loader changes from upstream; do not enable it globally for EPUB.

Line counts are the Pocket footprint inside each inherited file. Sites are
named by function, not line number — line numbers drift.

### `src/network/CrossPointWebServer.h` (~40 of 193 lines)

- `#include "pocket_daily/web/PocketWebServices.h"` — the umbrella header is
  the **only** Pocket include this file may carry.
- `using CrossPointWebServerProfile = PocketDaily::Web::Profile;` — keeps
  every existing call site compiling unchanged.
- Service members: `pocketStream`, `pocketHost`, `liveStudio`,
  `liveStudioHost`, `pocketRoutes`, `pocketTimeWait`, `clientActivityAt`.
- One-line decls: `wirePocketHost()` / `wireLiveStudioHost()` /
  `wirePocketRoutes()`, `shouldEndSession()`, `statusInputs()`,
  `repaintRequested`, `beginTransferFocus()` / `endTransferFocus()`.
- `UploadState` staging fields (part of exception E4).

### `src/network/CrossPointWebServer.cpp` (~150 of 2,172 lines)

Call-site hooks (each ≤10 lines):

- Includes: `direct_session.h`, `live_studio/DevTrace.h`,
  `upload_stream_protocol.h`; plus a 1-line `updateCrc32` shim into
  `PocketDaily::UploadStream`.
- `begin()`: profile route-gating `if` (E1); `wirePocketHost()` +
  `pocketStream.begin()`; `liveStudio.begin()`; `wirePocketRoutes()` +
  `PocketDaily::Web::registerPocketRoutes(*server, pocketRoutes)`.
  `hasBrowserRoutes` preserves FULL/FILE_TRANSFER; COMPANION omits browser
  route allocations and serves status at root. Profile selection uses the
  existing Pocket Sync launch mode; that mode also skips mDNS on X4.
- `stop()`: `pocketStream.stop()` (first — listener teardown precedes bye
  broadcast and `wsServer->close()`; keep the order).
- `handleClient()`: Sync-only `pocketTimeWait.service(...)` before the
  presentation-busy return (forced while admission is pending; see
  `sync-route-memory.md`); dev-trace heartbeat (`#ifdef`), stream-first service;
  skip HTTP while `pocketStream.receiving()` (HEADER/DATA), allow commit in
  REPLIED, and tick Live Studio even without a WS slot to finish its cooldown.
- `shouldEndSession()`: 1-line delegation to `PocketDaily::DirectSession`.
- `handleStatus()`: 1-line call to `PocketDaily::Web::buildStatusJson(statusInputs())`.

Thunk blocks (E3): `wirePocketHost()` (8 thunks), `wireLiveStudioHost()`
(5 thunks, includes the file-static `wsInstance` trampoline — the inherited
WS-upload grammar's event routing stays in this file), `wirePocketRoutes()`
(8 thunks, including the dev-only `timeWaitPurged` evidence counter).

### Wi-Fi initialization boundary (experimental build only)

The normal upload data plane also uses `SocketReceive.h` for allocation-free,
nonblocking descriptor reads into existing buffers. Its socket owner remains
NetworkClient; no buffered NetworkClient reads may be mixed into that path.

`src/pocket_daily/web/WifiInitBudget.cpp` supplies `__wrap_esp_wifi_init` only
when `POCKET_BOUNDED_WIFI_BUFFERS` is enabled. `sta_recovery` adds the linker
wrap flag; default and release retain the SDK path. The wrapper copies the
complete SDK configuration and changes only RX/TX buffer limits and the RX
block-ACK window, using no heap. Diagnostic buffers are separately gated by
`ENABLE_DEV_NETWORK_DIAGNOSTICS`, not by remote-flash availability.

### `src/main.cpp` (~30 of 755 lines)

- `#include "pocket_daily/boot/ProductBoot.h"`.
- Boot hooks after SD init: `PocketDaily::Boot::begin(renderer,
  deepSleepInProgress)` + `installJapaneseFont()`; `beginNetHealth()`;
  `applyStartupUiPack()` (also loads the Pocket Daily profile).
- Quick-resume wake: `paintWakeCue(renderer)` replaces upstream's corner
  `LoadingIcon` draw (and its include) with a full-width wake band.
- Routing: `consumeDevBootReturn()` else-if; silent-reboot else-ifs keyed on
  `kRebootTargetPocketDaily` / `kRebootTargetPocketNearbySync`; the
  read-and-clear bound uses `kRebootTargetMax`.
- Staged firmware: a `landsOnShell` flag set by the Pocket Daily and silent
  Home branches, then `PocketDaily::StagedFirmware::consumeOffer(...)` (every
  boot) pushes a staged-mode `SdFirmwareUpdateActivity` for `/update.bin`.
- Consumed developer-update return calls `goToFileTransfer(true)`; its one-shot
  flag skips only network mode selection and reuses saved-network association
  with the existing interactive failure path. Normal launches keep the chooser.

### `src/activities/network/CrossPointWebServerActivity.{h,cpp}` (~180 lines)

- `POCKET_NEARBY_SYNC` branches in `onEnter` / `onExit` /
  `returnToLaunchOrigin()` (≤3 lines each; the plan's E-exception shrank to
  these). `startUpdateCheck()` replaces the idle Sync menu with
  `OtaUpdateActivity(Origin::PocketDaily)`; `updateCheckHandoff` is the one
  `onExit` exception to the Nearby restart (no radio has started).
- `NetworkModeSelectionActivity`: the Pocket variant's third item
  (`NetworkMode::CHECK_FOR_UPDATES`), guarded by `scripts/test_sync_routes.py`.
- Private-AP start path policy calls: `nearbyStartAllowed()`,
  `nearbyReadyAllowed()`, `webStartAllowed()`, `selectProfile()`,
  `armPrivateApWatchdog()`, `generatePrivateApCredentials()`,
  `kPrivateApMaxConnections`, `apBootLog()` markers.
- STA loop: passive `StaRadioWatch` (`staWatch.onCheck(...)` switch) and
  `staWatch.cleanAssociation()` in the Wi-Fi indicator. There are no gateway
  probes or forced reconnect actions; the driver owns association recovery.
- The NearbySync state-machine drive and render stay here by design (already
  exemplary in shape; extraction is not a merge problem).
- `setContentPresentation(...)` thunk block: enqueue/receipt/busy plus
  `describe` (resolved display inputs via `ContentPresentation::describe`) and
  the developer `captureFrame` (`LiveFrameCapture::captureNow` under
  RenderLock, only for a completed content frame); one `LiveFrameCapture.h`
  include.

### Already-clean registrations

`ActivityManager`, `Home`, `UITheme`, `OtaUpdater` carry only registration-level
Pocket lines (member + one-line dispatch).

## Registered exceptions (E1–E5)

Pocket regions in inherited files that cannot be ≤10 lines:

- **E1 `begin()` route gating** — the structural `if (profile !=
  CrossPointWebServerProfile::POCKET_SYNC)` block around inherited route
  registration. Splitting it would touch every inherited `server->on` line;
  the single `if` is the minimal diff.
- **E2 `statusInputs()` mirror** — marshals host state into the value snapshot
  `PocketStatus` builds from. A value snapshot per field is what keeps
  `PocketStatus` free of inherited includes; it must grow when the snapshot
  grows.
- **E3 thunk blocks** — `wirePocketHost()` / `wireLiveStudioHost()` /
  `wirePocketRoutes()`. Each thunk is ≤3 lines; the wrapping function is
  pocket-owned code whose home is the inherited TU (thunks need private
  member access).
- **E4 chunked-upload staging patch** — the `state.chunked` / `state.chunkStart`
  continuation logic plus 8 two-line `pocketStream.publishHttpStaged(...)`
  hooks inside the inherited `/upload` handler and `UploadState`. The staging
  ledger (`StagedUpload`) is pocket-owned; the inline patch is what lets the
  inherited chunked HTTP path and the port-82 stream publish into one ledger.
- **E5 NearbySync drive + private-AP start path in the Activity** — the
  state-machine body and start ladder (BLE → private AP → gate → server) are
  product features in an inherited file. The plan accepted them as resident
  ("already exemplary"); policy details live in `PrivateApPolicy`.

The original plan's "route table data" exception landed entirely pocket-side
(`PocketEndpoints.cpp`) and is no longer an inherited-file exception.

## Carried-patch register

Product changes to inherited files that are behavior, not seam plumbing. On an
upstream merge these need semantic reconciliation, not mechanical re-hooking.

Upstream-PR candidates (generic repairs; extracting them upstream is a
follow-up, never send product stack upstream):

- `src/activities/reader/EpubReaderActivity` (~+380) — reader robustness.
- `src/network/HttpDownloader.{h,cpp}` (~+460) — download robustness.
- `lib/Epub/` (`Section.*` ~+820, `ChapterHtmlSlimParser.*` ~+325,
  `Epub.cpp` +35) — parser fixes.
- `lib/GfxRenderer/`, `lib/EpdFont/`, `lib/OpdsParser/`,
  `lib/KOReaderSync/` — parser/renderer fixes.

Fork-resident product patches (no upstream intent):

- `src/components/themes/{BaseTheme,lyra/*,roundedraff/*}.cpp` — runtime
  metric reads go through `UITheme::getInstance().getMetrics()`, including
  formerly constexpr button dimensions. Native constants remain in headers and
  UITheme's theme selection. This prevents draw/layout paths from bypassing
  active UI packs. Pagination uses Pocket's bounded `MetricGeometry` helpers;
  tiny rectangles never produce a zero page-count divisor. Keep the source
  boundary test `ThemeMetricBindings` when reconciling upstream theme changes.
- `src/network/FirmwareFlasher.{h,cpp}` — shared staging buffer + image
  validation used by Pocket transfer/diagnostics; `validateImageFile` takes an
  optional chunk observer (the staged prompt reads the image version in the
  validation pass). (The AgentDeck OTA decode and pulled-image hashing that
  also borrowed the buffer were removed 2026-09-25.)
- `src/activities/settings/{SdFirmwareUpdateActivity,OtaUpdateActivity}` —
  staged mode (preset `/update.bin`, no picker, same-version skip) and
  `OtaUpdateActivity::Origin::PocketDaily` (exits silent-restart into Pocket
  Daily with Wi-Fi off).
- `src/network/OtaUpdater.cpp`, `src/network/WebDAVHandler.cpp` — OTA channel,
  WebDAV guards.
- `src/CrossPointSettings.h` — Pocket preference fields.
- `src/main.cpp` — `SET_LOOP_TASK_STACK_SIZE`; RTC_NOINIT
  `silentRebootMagic`/`silentRebootTarget` words; the `SilentRestart.h`
  declaration set (Pocket implementations attach from `ProductBoot.cpp` via
  free-function definitions — see include rules).
- `lib/hal/` (`HalSystem`, `HalPowerManager`, `HalGPIO`, `HalStorage`) —
  product HAL features (breadcrumbs, sleep, button semantics). The AgentDeck
  timed-wake cadence (`startTimedDeepSleep`, `WakeupReason::Timer`) and its
  `main.cpp` half (`enterTimedDeepSleep`, the `timedSleep*` RTC words,
  `PowerCycle.h`) were removed 2026-09-25; upstream's wake handling is back to
  its own four reasons.
- `src/CrossPointSettings.h` / `src/SettingsList.h` — the AgentDeck toggles
  (`agentDeckCompanionEnabled`, `agentPullSyncEnabled`) were removed
  2026-09-25. `settings.json` is keyed, so older files load unchanged and the
  keys drop out on the next save.
- `src/RecentBooksStore.*`, `src/SdCardFontSystem.*`, themes/icons,
  `platformio.ini`, CI workflows, `README.md`/`USER_GUIDE.md` — product
  identity, build, and release config.

## Include rules (cycle prevention)

`pocket_daily/web/*` and `pocket_daily/boot/*` may include these inherited
`src/` headers and no others:

- `CrossPointSettings.h`
- `components/UITheme.h` (GUI global arrives via the theme chain)
- `network/FirmwareFlasher.h` (from pocket modules: full path
  `"network/FirmwareFlasher.h"`)
- `util/BookCacheUtils.h`

Forbidden: `network/CrossPointWebServer.h`, `activities/*`, `main.cpp`
internals, `SettingsList.h`. `wl_status_t` comes via `<WiFi.h>` —
`wl_defines.h` is not directly includable. Library HAL/Arduino headers
(`HalStorage.h`, `WebServer.h`, `<WiFi.h>`, …) are unrestricted.

`CrossPointWebServer.h` carries exactly one Pocket include:
`pocket_daily/web/PocketWebServices.h` (umbrella over the service modules).
Any new Pocket service joins the umbrella, not the inherited header.

Reverse attachment without includes: `ProductBoot.cpp` defines
`silentRestartToPocketDaily()` / `silentRestartToPocketNearbySync()` under
upstream `SilentRestart.h`'s existing declarations, so inherited call sites
compile with zero edits.

## Mirror obligations

Facts duplicated across the seam. When one side changes, the other must change
in the same commit:

1. **Status base fields.** `PocketStatus.cpp::buildStatusJson()` must emit
   every base field upstream's `handleStatus()` emits — currently seven:
   `version`, `ip`, `mode`, `rssi`, `freeHeap`, `uptime`, `device`. An
   upstream field addition must be mirrored here (this is why `/api/status`
   byte-comparison is part of the hardware verification bundle).
2. **Silent-reboot magic.** `ProductBoot.cpp`'s `kSilentRebootMagic`
   (0xC1EAB007) mirrors the constant on the RTC_NOINIT words in `main.cpp`.
3. **Reboot target values.** Upstream owns HOME=0 and READER=1; Pocket owns
   PocketDaily=2 and PocketNearbySync=3, with `kRebootTargetMax` as the
   read-and-clear bound. Renumbering breaks boot routing across the seam.
4. **Hidden-file rule.** `UiPackStore::listPacks()` mirrors `scanFiles`' dot
   rule (`SETTINGS.showHiddenFiles`); a change to one scan's rule should be
   reflected in the other.
5. **StaRadioWatch association semantics.** Repaint is always assigned, once
   on observed loss/recovery. Only actual sustained disconnection (>5 minutes)
   can abandon the activity. Application-port timeouts cannot request a radio
   restart. Elapsed-time subtraction is uint32 wrap-safe across one wrap.

## Invariants

- Hooks at inherited call sites stay ≤10 lines; anything longer is a
  registered exception or moves under `src/pocket_daily/`.
- Host reach-ins are plain function pointers (`Host`, `RouteDeps`,
  `LiveHost`) — no heap, no vtable, `-fno-exceptions`-safe. Wire them with
  captureless lambdas. Dependencies pass by pointer; value snapshots
  (`StatusInputs`) pass by value.
- Heap neutrality: extracting code must not add members to inherited classes
  beyond the service bundles, and must not cache borrowed buffers — the 4 KiB
  staging buffer (`firmware_flash::sharedStagingBuffer()`) is acquired at use
  and released on every exit path.
- Upstream-owned symbols stay defined in inherited files (RTC words,
  `wsInstance`, the WS upload grammar). Pocket code attaches via pointer
  bundles or free-function definitions; it never moves upstream symbols.
- The `stop()` order is contract: `pocketStream.stop()` → bye broadcast →
  `wsServer->close()`.

## Merge routine

1. `git fetch upstream && git merge upstream/main` — merge, never rebase
   product history onto upstream; never send Pocket product-stack files
   upstream.
2. Expect conflicts only at the hook sites and carried patches above.
   Resolution: take upstream's code, re-apply the named hooks verbatim.
   `src/pocket_daily/**` paths cannot conflict (upstream has none).
3. Verify every merge: `./scripts/pio.sh run -e default` **and**
   `-e gh_release` (the dev-endpoint `#ifdef` surfaces differ), plus host
   tests (`cmake --build build/host-tests && ctest --test-dir
   build/host-tests -j 4`; 157 tests at time of writing). Run
   `./scripts/pio.sh check` (strict cppcheck) at sprint closeouts.
4. If upstream changed `handleStatus()` fields, the silent-reboot constants,
   or the upload state machine, honor the mirror obligations above in the
   same merge.
5. Hardware-sensitive claims (status bytes, route matrices, transfers) need a
   device session — simulator/host results do not verify them.
