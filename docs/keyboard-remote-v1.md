# Keyboard remote over BLE v1 (REMOTE1)

Local development feature; release and distribution remain paused by owner request
(2026-11-01). Both app and reader start OFF. No preference, boot marker, sleep
wake or background reconnection enables a shipping remote session.

## Use

Pair once using the existing Nearby Sync passkey flow. In an EPUB reader menu,
choose **Enable keyboard remote**; in Pocket Daily → Manage Reader, enable
**Keyboard Remote** for that paired reader. Focus the control area to use Left
(previous), Right or Space (next). Button controls use the same commands.
Keys outside that focus area and modified/repeating keypresses are not captured.

Opening the reader menu, closing the book, disconnecting, leaving the app panel,
backgrounding the app, five minutes without a turn, two hours total on the
reader, battery at/below 10%, or insufficient reading memory ends the session.
Enable again explicitly. Sleep wake remains a separate feature.

## Transport and ownership

Uses the existing Nearby Sync service and encrypted/MITM authenticated command,
status and event characteristics. REMOTE admits stored bonds only, using the
existing unresolved-private-address grace limit; it cannot create a new bond.
Status carries `V=1;MODEL=...;ID=...;FW=...;CAP=REMOTE1;WIN=1;RS=<eight-uppercase-hex-session>`. It deliberately
omits READ1, WIFI1 and AP so the background reading-sync client cannot own it.
The app checks the full paired reader ID and REMOTE1 before accepting input.

Commands: `NEXT 00000001` / `PREV 00000002`. IDs are eight uppercase hex digits,
strictly increasing from 1 per explicitly enabled session. Only one turn may be outstanding.
After authenticating, the app sends `REMOTE_READY 00000000`; only an unused
session accepts this initial handshake. The reader creates a RAM-only RS nonce.
NimBLE callbacks enqueue bounded records; the reader loop invokes the same
logical `pageTurn` path as local buttons, subject to the 200ms/render guard.
No SD/settings write occurs in a BLE callback or for each keypress.

Finalized same-chapter text pages stream complete absolute grayscale planes
through a reusable 4096-byte scratch buffer while BLE stays connected. Images,
unsupported modes, chapter preparation and raster/cache recovery use the slow
path: `LAYOUT <id> <RS>` announces a bounded radio pause. The app reconnects only the same peripheral and verifies the same full
identity and RS, then sends `REMOTE_READY <id>`. It never resends NEXT/PREV.
An unexpected disconnect or changed session stops the app remote.

After the page renderer finishes and the same BLE session is ready again: `PAGE <id> <readerElapsedMS> <freeHeapBytes>
<largestBlockBytes>` (one line). This measures command consumption through
render completion, BLE reinitialization and readiness, **not optical latency**. The app also measures command-send
to acknowledgement receipt; neither value isolates radio latency.

`ERR <id> BOUNDARY|BUSY|STALE|REMOTE_ONLY|SESSION_CHANGED` is not retried automatically. A replay
of the latest completed ID returns its cached PAGE without another turn. Older
IDs and a duplicate pending ID are rejected. Invalid/ambiguous responses stop
the app session; the user checks the page before reconnecting. CANCEL closes
the reader session. Ordinary app Stop disconnects, which also closes it.

## Resource admission

OFF retains only small module state; the Service/runtime and NimBLE are allocated
per session and deinitialized on exit. Prepare the chapter and paint the page with the radio off. Release rebuildable
font caches and the primary framebuffer before BLE startup; the physical e-paper
retains its last image. Fast text rendering writes bounded strips directly to the
panel's complete absolute planes, with one combined activation after successful
rasterization. It allocates no second full framebuffer. Glyph/font preparation
requires additional 16KiB free headroom, and memory is checked after prewarming
and each band. Incomplete planes are never activated. The fallback fully tears
down BLE, restores the framebuffer, repaints and reconnects the same RAM session.

Render-visible session transitions and scratch/frame lifetime are serialized by
RenderLock. Frameless rendering requires a pending, initialized streaming frame;
cache/error/overlay/section preparation requests fallback before ordinary drawing.
Optional chapter prefetch is disabled while remote is preparing or active.
Physical buttons stop remote and restore the framebuffer. Restoration allocation
failure records a breadcrumb and reboots safely. Existing SDK framebuffer loans
are unchanged. Initial gates remain 130KiB free / 40KiB largest block, reserving
~80KiB for BLE startup and its worker; active gates remain 50KiB / 16KiB.
Never lower them to obtain a passing hardware result.

With the normal clock backend, connected idle requests 80MHz (BLE-safe floor),
active work 160MHz. The PM-lock/ENABLE_BLE_STANDBY backend conservatively retains
its maximum clock while BLE is required. Clear the BLE clock requirement only
after teardown. Streamed refresh powers panel rails off between turns. These
policies do not imply measured battery savings.

## Developer measurement (never a production API)

The existing dev reader-capture endpoint accepts `action=keyboard-remote` for
an owned `Pocket visual <run>.epub` fixture. It returns automatically to Same
Wi-Fi after 480 seconds: 30s OFF, up to 420s ON, 30s OFF. No synthetic progress
is published. It samples private `/.pocket/dev-remote.jsonl` every 5s with run,
time, BLE-active/preparing state, admission failure reason, free/largest heap,
battery percent, cumulative checkpoint minFree/minBlock, streamed-frame count
and, only on X3,
signed fuel-gauge mA. Negative mA means discharge. Save/read back evidence and
restore the original diagnostic files; never overwrite an existing trial log.

The app hardware test discovers an enrolled advertiser, then uses the real app
transport/controller and shipping keyboard control view. Its JSON attachment
keeps per-turn completion/round-trip/heap evidence. External measurement is
unavailable for this run: X4 percent changes cannot establish current or battery
life. Short X3 samples also do not establish long-term standby life. Private
backups, exact developer image/ELF/MAP and receipts remain under
`build/keyboard-remote-20261101/`; app xcresults under
`.build/keyboard-remote-20261101/`. Hardware results are recorded separately.

## Observed acceptance — 2026-11-01

X3 developer `1.0.0-dev-codex-keyboard-remote-a3fd9694-w319d57e0` passed
4 actual focused Mac keys plus 10 forward/10 backward turns (24 total).
Send-to-ACK median improved 4.789s → 1.601s (66.6%); 22 fast turns had a
1.600s median. Two chapter fallbacks took 5.510s and 5.818s, also p95/max.
Minimum ACK free/block: 73,948/55,284B. Cumulative checkpoint minima including
preparation: 56,824/40,948B. The full 94-sample window had no admission failure
and recorded 22 streamed frames. These are sampled/checkpoint minima, not a
proof of every instantaneous heap allocation. No optical timing was measured.

The new power recording shows positive charging current (47–87mA), so it cannot
establish consumption reduction against the prior battery-only baseline.
That earlier baseline was awake OFF 23–24mA discharge and connected idle 49mA;
the completed USB-disconnected comparison is recorded below. No external meter
or long-term battery-life measurement is available.

X4 stays code-only by owner request, without new pairing, flashing or turn tests.
Final default build, strict cppcheck, 1,166 host tests, 35 Mac and 20 iOS app tests
passed. Pixel-oracle tests cover all four orientations, AA modes and strip sizes;
physical coverage remains this X3/Mac fixture, not all books or orientations.

New private evidence: `build/keyboard-remote-perf-20261101/`, including frozen
`artifact-opt4/`, source hashes, original backups, power JSONL and restoration
receipt. App xcresult and summary: `.build/keyboard-remote-perf-20261101/` in
sibling `pocket-daily`. Prior baseline: `build/keyboard-remote-20261101/`.
Failed opt1 skipped render dispatch; opt2 restarted after eight fast turns with
no established root cause. Evidence is retained separately. Final opt4 completed
24 turns and the full recording window after render ownership/cache-recovery/
prefetch hardening; longer stability testing remains useful.

Battery-only follow-up: after owner USB-removal confirmation, unchanged opt4
completed 25 turns (four keyboard events + one button +20 automated), median
1.599s, p95 5.348s, max 5.825s. Of 92 discharge-current samples, fixed 10–25s
OFF median was −24mA (3), 45–70s connected idle −47mA (5, range −47..−46),
455–480s OFF tail −23mA (5). Earlier connected idle was −49mA: 2mA/4.1% lower
observed discharge, with no externally calibrated/randomized causal claim.
Enabled idle still costs about 23–24mA over awake OFF; no sleep/life estimate.
Checkpoint minima 56,824/40,948B, 23 streamed frames, no recorded admission
failure. ACK minimum free/block 73,908/55,284B. Fresh private evidence/backups:
`build/keyboard-remote-power-20261101/`. No reflash, update.bin mutation or X4
operation occurred. Source and all safety gates are unchanged from final opt4.

App PR review hardened pairing changes and fatal ERR handling; those final app
fixes passed deterministic Mac/iOS regressions, without another physical run.
Firmware source bytes still match the opt4 hardware source pin.

Merged into main via firmware #26 (`fb9a900a`) and app #19 (`84b96d2`).
Feature worktrees/branches were cleaned after preserving all four named evidence
folders under the original firmware `build/` and app `.build/` roots. Their PR
evidence folders hold migration SHA inventories; firmware pipeline reports are
nested under `build/keyboard-remote-pr-20261101/dev-pipeline-runs/`. The existing
SDK storage patch remains in the original firmware checkout. No new image was
installed during merging; release/distribution remains on hold.
