# Mode Lifecycle Resource Separation — design note (2026-09-20)

Status: **design only**. No new implementation in this note. The next
implementation increment waits on HN-2 heap-map evidence from a device session
(see the evidence protocol). Related: `SEAM.md` (where the hooks live),
`network-stability-analysis.md` (why heap pressure matters),
`nearby-sync-v1.md` (the BLE memory gate), `PROJECT_MEMORY.md` (2026-09-20
sprint entry).

## The problem

The X3 has no PSRAM. Every mode — reader shell (Pocket Daily), File Transfer,
Nearby Sync (BLE + private AP) — needs large contiguous internal-RAM blocks,
and they cannot all be resident at once. The observed costs of ignoring this:
uploads losing sockets at ~128 KiB, NimBLE preflight failures bouncing back to
Pocket, and a File Transfer settle heap around 8 KiB on v1.6.6.

The goal is not a general heap diet. It is a lifecycle rule: **a mode owns its
resources from entry to exit; entering a mode may not inherit the previous
mode's large allocations; leaving a mode returns them.**

## What a "mode" is, and where its boundaries already are

| Transition | Mechanism today | Boundary quality |
|---|---|---|
| Reader shell → Nearby Sync | `silentRestartToPocketNearbySync()` — reboot | Full separation (landed; in-process NimBLE preflight was unreliable on the fragmented heap) |
| Web activity → Pocket | `silentRestartToPocketDaily()` in `returnToLaunchOrigin()` | Full separation (landed) |
| Reader shell exit with Wi-Fi used | defrag restart (`silentRestart` / `silentRestartToReader()`) | Full separation (landed) |
| Home → File Transfer | `ActivityManager::goToFileTransfer()` — in-process `replaceActivity` | Partial: SD font released at `startWebServer()` preflight |
| Reader shell → File Transfer (via Home) | in-process chain | Partial: same preflight release |

The reboot-based edges are the model to preserve: ESP.restart() is the one
teardown that is guaranteed complete on this heap. Any new mode edge should
default to the silent-restart pattern rather than trying to unload in process,
unless the resource has a proven release path (see inventory).

## Resource inventory

Reader-dedicated (must not be resident during transfer/BLE):

- **Resident SD `.cpfont` family** — interval tables, decompression pages,
  glyph caches in internal RAM. Release: `sdFontSystem.releaseLoaded(renderer)`
  under `RenderLock` (keeps the saved selection; reader reloads on demand).
  **Landed** at both entry preflights: `startWebServer()` and
  `startNearbySync()`.
- **Font/glyph caches** (`FontCacheManager::clearCache()`) — released as part
  of the family release path; standalone use only if a resident-family
  measurement says otherwise.
- **Book section arenas / parser state** — owned by the reader activity and
  destroyed with it (`replaceActivity`). Not separately managed today.
- **Startup UI pack metrics** — applied to `UITheme` at boot
  (`ProductBoot::applyStartupUiPack()`), buffer freed immediately; resident
  cost is metrics only. Cheap; leave as is.

Transfer-dedicated (must not be resident in reader shell):

- `CrossPointWebServer` + `WebSocketsServer` + TCP buffers — activity-scoped,
  destroyed at activity exit, and the Wi-Fi edges reboot anyway. **Landed.**
- Port-82 upload stream + 4 KiB borrowed staging buffer —
  `UploadStreamServer` acquires at use, releases on every exit path (SEAM
  invariant). **Landed.**
- WS push listener vs uploads — `LiveStudioService` transfer focus
  (suspend/resume listener; uploads own the DMA pool). **Landed** (first
  deliberate increment of this design).
- `FontUploadState` 4 KiB buffer — lazily allocated `unique_ptr`; verify it is
  freed on upload end (evidence item E-3).

Boot-loaded, mode-independent:

- **`installJapaneseFont()`** — publishes the embedded cpfont to SD with a
  revision check. Not a RAM cost, but it runs on every boot *before* mode
  routing, including silent reboots into Nearby Sync where nothing reads it.
  Candidate deferral: run only when booting into the reader shell (see
  increments below). Resume-latency effect unmeasured.
- NetHealth ring, RTC_NOINIT words — fixed-size, mode-independent by design.

## Candidate next increments (in priority order, all pending evidence)

1. **HN-2 evidence matrix first.** Before any new release code, capture
   `dev/heap-map` + `dev/stack-report` per mode × phase (boot settle, transfer
   settle mid-upload, nearby pre/post-AP, reader idle after book open). The
   disease map says "File Transfer settle ~8 KiB" — this matrix says *what* is
   still resident there, which picks between increments 2–4. Sniper pattern:
   short radio windows, evidence pulled via the SD bridge or a one-shot
   request, not a long interactive session.
2. **Boot deferral of `installJapaneseFont()`.** Move the SD publish from
   unconditional boot into the reader-shell boot path (the `ProductBoot` hook
   already sits next to mode routing). Effect is time (silent-resume latency),
   not heap — measure the SD publish cost on device before deciding. Risk:
   first-run flow expects the font present; keep a lazy fallback in the reader
   shell.
3. **File Transfer settle-heap reduction** (needs increment 1's data): if the
   matrix shows a specific resident block during transfer that no transfer
   feature uses, attach its release to the existing `startWebServer()`
   preflight — the hook already exists and is registered in SEAM.md. No new
   seam sites.
4. **Ladder exhaustion → mode reboot.** The STA radio ladder
   (`StaRadioWatch`) currently ends at `esp_wifi_stop/start`. The registered
   follow-up (network-stability-analysis.md) is a final escalation that
   reboots into File Transfer. This is mode-lifecycle-as-recovery: the reboot
   boundary doubles as the guaranteed release. Design constraint: it must
   preserve the resume ledger (`StagedUpload` banking already survives
   restarts) and never look like a crash to the companion.

## Non-goals

- No general heap diet, no allocator changes, no protocol changes.
- No in-process mode unloading beyond resources with proven release paths
  (the reboot boundary stays the default).
- No new inherited-file touchpoints: every increment attaches to an existing
  seam site (`ProductBoot` hooks, `startWebServer()` preflight,
  `LiveStudioService` focus) or lives pocket-side.
