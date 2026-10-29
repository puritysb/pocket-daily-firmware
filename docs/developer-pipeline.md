# App–reader developer pipeline

The local runner is `scripts/dev_pipeline.py`. It builds the real Debug app,
uses its actual `PocketModel` and Bluetooth ownership, and records a hardware
scenario alongside firmware checks and the exact candidate image. It adds no
reader menu, persistent server, firmware heap allocation or production remote
control interface. The app's weak test-model reference compiles out of Release.

`run --app <checkout> --interface-suite` adds the app-owned ten-case interface
suite: identity rejection, real SD usage, settings/profile readback and restore,
books/articles and downloads, downloaded-book pagination, pause/resume/discard,
Mac folder copy, and inventory preservation. Evidence must contain every named
case with an accepted terminal state; skipped/missing cases are failures. See
the sibling app's `docs/READER_INTERFACE_VALIDATION.md` for the full coverage
matrix and `scripts/verify_reader_interface.py` aggregate entry point. This flag
does not imply `--flash`, and no normal app or reader menu input is needed during
the run after the initial enrollment/permissions prerequisites are met.

## First setup

Use a development firmware with the existing remote-flash endpoint. Connect the
reader to Same Wi-Fi, and pair the signed Debug companion once if testing BLE
wake. Grant the ordinary Bluetooth/local-network permissions on the test Apple
device. Do not use Demo mode. Quit a separately running companion before testing
so the XCTest-hosted app is the only owner of the BLE connection. The runner
checks for another Pocket Daily process before contacting the reader and again
before hardware work, and stops rather than contending with its heartbeat or an
active transfer. During an agent-led loop the agent can close the normal app
once, then repeat the CLI without further app or reader menu operations.

The host needs Xcode with a signing identity, XcodeGen, CMake, the repository's
native PlatformIO wrapper, and cppcheck 2.11 for full firmware gates. Use
`--cppcheck <path>` if the pinned analyzer is installed elsewhere.

`--checks` runs the complete host suite, every `scripts/test_*.py` test
(including ownership-boundary checks), strict analysis and the default build.
Restricting Python discovery to transport tests misses repository-wide gates.

From the firmware checkout:

```sh
python3 scripts/dev_pipeline.py configure --host <reader-ip>
python3 scripts/dev_pipeline.py run
```

Enrollment stores only the LAN address and expected reader identity in ignored
`build/dev-pipeline.json`, mode 0600. Runs reuse it; a different reader is rejected.
When the saved address times out, returns malformed status or belongs to another
reader, the runner starts `ReaderHardwareTests/testDiscoverEnrolledReader` in the
real Debug Mac app. It uses the normal Connect flow, including bonded BLE wake,
Bonjour and bounded current-LAN discovery. Its admission gate requires the
already enrolled identity, Same Wi-Fi, developer firmware and HTTP port 80 before
accepting a session. A fresh run-tagged receipt is required; missing, duplicate,
stale, skipped or failed discovery is not success. The runner then independently
rechecks HTTP identity/version before atomically updating only the saved host.
The rest of the requested inventory/transfer/standby suites proceeds normally.

The generic fallback uses the app's startup-selected model. It cannot discover
an enrolled X4 through a selected X3 registration's identity gate, and a visible
UI selection does not replace that test bridge. Do not weaken the gate or
remove/reorder registrations. For this case the Mac Debug
`ReaderX4SettingsTests/testDiscoverEnrolledReader` provides an explicit read-only
LAN search using an isolated model, empty Bluetooth remembered store and no-radio
transport. Its required test-host flags are `POCKET_HARDWARE_ENABLED=1`,
`POCKET_HARDWARE_DISCOVERY=1` and `POCKET_HARDWARE_MODEL=X4` (passed to Xcode with
`TEST_RUNNER_` prefixes), plus the existing private enrolled identity, old host
hint and fresh nonzero run. It accepts only that X4 in STA/developer/HTTP-poll
mode on port 80, then verifies a fresh HTTP response. No settings/content/upload,
flash or Bluetooth wake command is sent. The selected app model remains paused
until test-host exit; its registration is preserved.

Follow the [Mac invocation and private environment pattern](../../pocket-daily/docs/DEVELOPER_PIPELINE.md#x4-lan-discovery-before-installation).
Validate the single run-tagged `POCKET_DISCOVERY_RESULT`, independently recheck
HTTP identity/model/version, and update only the config host hint atomically.
This is not an automatic X4 fallback in this runner. Discovery failure must not
change enrollment, and discovery success does not establish hardware acceptance.

For the matching selected registration, no IP input or re-enrollment is needed
for a network/DHCP address change. Reports
record `discovery.method` (`saved-address` or `app-discovery`) and whether the
address changed; the discovery log and `.xcresult` stay in the private run folder.
The discovery scenario deliberately seeds the app with the runner's old address,
and restores its previous address hint on failure. Enrollment remains unchanged
if discovery, receipt validation or the independent HTTP check fails.

The runner never switches the Mac/iPhone to the reader's private hotspot or
copies Wi-Fi credentials. The reader must already know/join the current network;
use the app's Wi-Fi provisioning when credentials change. A powered-off reader,
deep sleep, low-battery BLE cutoff, denied OS permissions, network isolation or an
unreachable subnet can still prevent discovery. This does not bootstrap release
firmware. An initial enrollment still requires the reader's address once.

## Iterate

For a real delayed-advertising recovery experiment, use `--standby
--delayed-advertising`. The developer `ble-cycle` request accepts
`delayAdvertising=1` only with standby. It defers BLE startup until
`(standbySeconds + 16) * 1000` milliseconds into standby while retaining battery,
button and automatic return handling. The versioned/checksummed developer record
reports `advertisingDelayMs` and `advertisingStartedMs`; the record gains eight
bytes, no additional heap buffer, and its magic changes to reject old layouts.
The fault has no persistent setting and is compiled out without
`ENABLE_DEV_REMOTE_FLASH`.

The app-owned harness requires pending connection cancellation and confirmation,
actual service scan, enrolled advertisement, one reconnect, authentication and
exactly one acknowledged wake, in that order and
inside the original 20-second app deadline. `--wake-control` suppresses only the
app's eight-second recovery action via a test-only scheduler. It preserves the
real radio and can succeed without rediscovery; that result is a control, never
recovery evidence. Both paths still check inventory, EPUB bytes, cleanup and idle
return. An old image that ignores the new query fails evidence validation.
Use the sibling app's `docs/CURRENT_HANDOFF.md` for measured results and limits;
injected outages do not establish an unobserved historical RF cause.

Fast app iteration against the installed reader:

```sh
python3 scripts/dev_pipeline.py run --standby --cycles 2
```

Full firmware/app iteration, including developer installation:

```sh
python3 scripts/dev_pipeline.py run \
  --build-firmware ble_standby --flash --standby --cycles 2 \
  --ios-simulator <available-simulator-udid>
```

Use `--build-firmware default` without `--standby` for ordinary firmware. The
experimental indefinite BLE standby scenario is **X3 only** and needs the
isolated `ble_standby` SDK. `--checks` runs the full firmware host suite, pipeline,
upload and route tests, strict pinned cppcheck and default firmware build.
`--build-firmware` includes these gates. Run `./bin/clang-format-fix` and inspect
the diff before a C++ iteration; the pipeline does not rewrite source formatting.

To use an already built artifact:

```sh
python3 scripts/dev_pipeline.py run --firmware firmware/update.bin --flash --standby
```

`--flash` is explicit developer installation, separate from normal app update
confirmation. The image is frozen under the run directory before installation.
The normal acceptance path builds the Mac app before preflight so it can
discover an unreachable saved IP; `--install-only` skips both steps.
All requested firmware/app build and test gates precede installation. An exact
version already installed skips flashing. Otherwise upload uses the existing
port-82 acknowledgement/commit protocol, sends the developer flash request once,
and verifies the same reader and exact embedded build version after reboot.
Neither an ambiguous installation nor a standby request is blindly replayed.

### Installation without hardware acceptance

To install a prevalidated frozen developer image before inspecting native X4
settings, keep the X4 enrollment separate and use:

```sh
python3 scripts/dev_pipeline.py run --app ../pocket-daily \
  --config build/dev-pipeline-x4.json --firmware "$FROZEN_FIRMWARE" \
  --flash --install-only
```

`FROZEN_FIRMWARE` is the absolute path to the unchanged local artifact. This
requires `run`, explicit `--firmware` and `--flash`; other scenarios, iOS,
builds/checks and nondefault cycle options are rejected. It performs no app
build, discovery or hardware test. The private config must already contain a
freshly identity-verified reachable address; stale addresses fail closed.
The existing exclusive lock, stopped-companion checks, image snapshot,
enrolled identity/STA/developer preflight, one-shot flash and exact post-reboot
version check remain. An already matching version skips flash.

The result has `installationOnly=true`, `acceptancePerformed=false` and
`hardware=[]`. Save pre/post state separately and record the installer source
hash independently from the frozen firmware's build provenance. Installation
success is not app, native-setting, reading, sleep or other hardware acceptance.
Inspect native settings before starting a suite that changes them.

`--cycles N` repeats the scenario 1–20 times and stops on the first failure.
This is a bounded validation loop, not a process that generates or edits code.
After diagnosing and fixing a failure, rerun the same command for fresh evidence.

## What the hardware scenario proves

The shared app source is `HardwareTests/ReaderHardwareTests.swift`, included in
both unit-test targets. It skips with no device I/O unless explicitly opted in
via the runner's `TEST_RUNNER_POCKET_HARDWARE_*` environment. It fails if enabled
on a simulator or if required configuration/identity/version is missing.

1. Confirm development firmware, expected identity and Same Wi-Fi.
2. Call the running app's normal Connect flow; await its actual connected state
   and a newly refreshed inventory. No mocked transport or second app model.
3. With `--standby`, request one uniquely numbered developer sleep cycle, wait
   until off Wi-Fi, then invoke the same Connect flow. This exercises the actual
   bonded BLE wake, reader reset, saved-network rejoin and app reconnect.
4. Match the durable firmware run number and require successful `app-wifi`
   close, actual light sleep, framebuffer recovery and BLE heap thresholds.
5. Read root and `/Books` through the actual app client to terminating cursor
   zero, so a swallowed optional-folder error cannot produce a green result.

Fresh inventories and folder counts are recorded; book names/bytes/passkeys are
not put in the structured hardware evidence. Xcode's raw local logs can contain
network identifiers: treat the ignored run directory as private, review before
sharing, and never commit it.

For an attached, unlocked, provisioned iPhone with the Debug app already paired:

```sh
python3 scripts/dev_pipeline.py run --standby --ios-device <physical-iphone-udid>
```

This adds the same scenario on that physical iPhone after the Mac scenario.
`--ios-simulator` builds/tests shared wake and protocol logic but **does not**
verify radio, local-network permission, iPhone sleep/background behavior, or a
physical iOS connection. The runner never labels simulator success as hardware
success. X4 and current/battery-life measurements remain separate gates.

Production public-API acceptance uses the app's separate production opt-in,
not this developer runner. Its Mac Debug X4 branch has an isolated temporary
registry and NoRadio model while retaining the shipping fleet identity guard;
the startup-selected reader stays paused. It requires the production reader to
already expose Same Wi-Fi and does not enter Sync after a production USB boot.
X3 and physical iOS retain their startup-selected model behavior. See the
[app production test contract](../../pocket-daily/docs/DEVELOPER_PIPELINE.md#production-firmware-explicit-public-protocol-acceptance)
for backups, exact identity/version/capabilities and restoration limits.

## Reports and failure recovery

Every run prints `build/dev-pipeline-runs/<timestamp-id>/report.json`. It is
updated after each stage, including failures/interruption. The directory holds
stage logs, candidate SHA-256/version/size, source commit and working-change
digests, XCTest `.xcresult` bundles and per-cycle measurements. The run directory
is private (0700); generated files are private (0600). An exclusive lock prevents
two instances of this runner from operating the checkout simultaneously; do not
run manual PlatformIO, upload or other reader tools alongside it.

A green xcodebuild exit is insufficient: exactly one matching, passed hardware
record must exist per scenario. Skipped tests, stale cycle IDs, changed versions,
missing sleep metrics, and low BLE heap fail the run. Pipeline tests inject these
faults and test timeout/lock/failure persistence.

On a failed stage, inspect its log and report. Do not immediately repeat an
unknown flash result. Inspect `/api/status` and the expected artifact identity
first. A timed-out developer standby trial has a bounded firmware fallback to
saved Wi-Fi; wait for that return before restarting the pipeline. Physical
recovery remains necessary if the reader can no longer boot or join its network.

### Interrupted standby evidence (2026-10-09)

Developer cycles persist checkpoints before sleep preparation, after framebuffer
release, before automatic light sleep, when the BLE radio is ready, and every
30 seconds while the opted-in trial is active. `checkpoint` is respectively
1, 2, 3, 4, 5 (0 means none); `batteryPercent` is the gauge reading at that
checkpoint. These extra writes are developer-trial diagnostics, not a battery
benchmark or the normal standby path. The checked SD record is at most 208 B
and its format magic is bumped; archive an older result before installing this
build. A reboot-interrupted record keeps its last checkpoint and memory sample.
`state=4` is a failed/interrupted trial, never a completed wake. Zero fields with
`statsValid=false` cannot establish BLE admission or a successful radio start.

X3 GPIO retention: the inherited standby SDK enables `CONFIG_PM_SLP_DISABLE_GPIO`.
[ESP-IDF v5.5.5 Kconfig](https://github.com/espressif/esp-idf/blob/v5.5.5/components/esp_pm/Kconfig#L73-L84)
specifies that this disables GPIO during automatic light sleep, with
`gpio_sleep_sel_dis()` as the per-pin override. The X3 standby path keeps SD
mounted and polls buttons; its SD supply enable (GPIO13), chip selects and input
pulls must retain their normal configuration. `HalPowerManager::beginBleStandby`
now applies that override to valid GPIOs before enabling automatic light sleep,
and fails admission if the override fails. Normal terminal deep sleep continues
to explicitly turn rails off and isolate pins. Current savings are unmeasured.
The app hardware trial now waits 90 seconds before Connect, crossing the first
minute's battery check instead of exercising only the first few seconds.

`run --app <checkout> --wifi-setup` tests app-entered Wi-Fi through the actual
Debug app and production encrypted BLE command. The reader needs WIFI1 firmware
and the app's existing bond. A unique nonexistent SSID deliberately fails; the
runner requires the app to confirm failure and return to the same reader on its
previous network. It never obtains real Wi-Fi passwords. Successful new-network
provisioning is a separate test requiring a reachable network supplied by the user.

## Development screen capture

`run --app <checkout> --config build/dev-pipeline-x4.json --x4-sleep` is the
separate actual-X4 Home/Brief/deep-sleep scenario. It requires external USB
power detected by the HAL, retains the normal X4 battery-latch shutdown, and
adds only a developer three-second timer. External power is rechecked at timer
arming; battery-only X4 requests are rejected. Do not disconnect the cable.
No USB monitor, physical button actuator or altered production power path is used.

Seven cases cover all four Home/three Brief presets, WAKE on/off and cover on/off.
The developer `sleep=timer-home` mode (returnMode3) first follows normal startup
routing after wake, saves the actual returned Home, then switches to Same Wi-Fi
without another reset. Each requires real Pocket Daily activity frames, HAL timer-arm and timer-wake
receipts, matching reset reason, original identity/version, exact app preview
pixels, persisted settings/cards and unchanged body on wake. X4's documented
bottom waking-status band changes independently of its optional sleep WAKE tab.
The runner exports49 hash-verified PNGs and rejects missing frames or restoration.
Developer-only home/sleep PDLI files retain the same paint's inputs on SD; they
allocate no second framebuffer and are absent from Release.

This establishes externally powered CPU deep sleep/timer return. Battery-only
latch power-off, physical power-button wake and optical panel quality remain
separate physical checks. The X3 screen suite below remains X3-specific.

`run --app <checkout> --screen-suite` runs the companion-owned physical X3
Home/Brief/card/EPUB/sleep tests (13 captures), exports BMP/PNG evidence through XCTest attachments with hashes,
and runs the app-owned local Vision OCR checks. The runner does not read the app's
sandbox directory. Missing/ambiguous attachments, hash mismatches or failed text/
layout anchors fail the run. This is distinct from
app screenshots. The endpoint is compiled only with `ENABLE_DEV_REMOTE_FLASH`;
there is no shipping preview feature or extra framebuffer allocation.

`/api/pocket/v1/dev/screen-capture` requires the enrolled `deviceID` and nonzero
32-bit `run` on every operation. POST also requires `surface=home|brief|card` and
`generation`. It accepts only the same completed normal-polarity presentation
and current profile generation (cards bind their active generation and `revision`),
takes a nonblocking RenderLock, saves the
retained framebuffer using the existing screenshot encoder to the fixed developer
path `/.pocket/dev-screen.bmp`, then returns 204. A failed save invalidates the
previous capture. GET requires `offset`, returns at most 1024 bytes and headers
`X-Capture-Run`/`X-Capture-Size`; no arbitrary file path is accepted. DELETE removes
that run's snapshot and invalidates its token. A checked 8-byte run ticket survives the controlled reader/sleep reboot; a partial
or corrupt ticket, failed capture or mismatched run cannot certify an older frame.
Wrong identity, generation, unfinished rendering or mismatched run cannot return
valid capture evidence. All operations reject active transfers. Downloads reuse
the existing staging buffer and bounded socket timeout. Saved metadata is
an 8-byte run/check ticket; no new full-screen buffer or background capture task is introduced.

Captures are local private diagnostic data. The host keeps failed artifacts and
restores the original profile/cards even when a screen check fails.

`POST /api/pocket/v1/dev/reader-capture?deviceID=...&run=...` is development-only.
It accepts only the existing `/.`-free root fixture `/Pocket visual <run>.epub`,
1 byte..4 MiB, and writes a checked request plus one-shot `R` boot marker. Before
opening the real EPUB activity it pre-arms the usual `S` Same Wi-Fi recovery.
It waits for a completed reader page under the rendering lock, saves its native
framebuffer and restarts to the saved network; no extra framebuffer, server or
render task. The 90-second failure route also returns to Wi-Fi. This isolated
fixture does not update APP_STATE's open book or recent books. A crashed trial
consumes its request and returns to Wi-Fi without repeating the open. The app
sends the generated fixture via its normal transfer job, removes only that file
and compares the original reading list after restoration.

For `sleep=timer`, the existing developer sleep cycle now saves the actual sleep
frame under its run number before framebuffer release. The app requires both
`timerArmed` and the HAL's `timerWake` after automatic LAN return. This is actual
X3 deep sleep, separate from experimental BLE light sleep; BLE requires battery
strictly above 10% (above 5% on X3 while charging is confirmed), and neither case measures battery life. No new shipping sleep mode or
remote-control endpoint is enabled. These framebuffer captures cannot establish
optical panel quality, grayscale overlays or physical button navigation.

### Long standby diagnostics

`--standby --standby-seconds 1800` requests a 30-minute uninterrupted app-side
wait. Values are bounded to 90..1800 seconds; firmware recovery is the requested
wait plus 90 seconds, persisted with the one-shot request. This needs matching
firmware. The report checks the exact requested interval, measured standby time
(at least wait minus 30 seconds for boot/teardown), and extended recovery limit.
Shorter or stale results cannot certify a long run. The app harness suspends
LAN monitoring and automatic BLE reading exchange during the wait, checks that
exchange history stayed unchanged, and restores ownership before normal Connect.
No test keepalive is sent during this wait. `chargingAtCheckpoint` describes the last recorded charging sample;
`low-battery` closes are distinct from timer expiry. The record magic changed,
so archive old cycle results before installing. A nonresponding reader may still
need one physical wake to retrieve evidence; an e-paper standby image alone does
not establish that BLE is powered.

Physical Mac/X3 acceptance on 2026-10-11:
`build/dev-pipeline-runs/20261011T105840-d292e9/report.json` passed a requested
1800-second idle interval. Measured standby was 1,807,465 ms, automatic light sleep
1,472,430 ms, one BLE connection, zero READ_LISTs, and `app-wifi` close. The app
reconnected in 12.223 seconds and completed its generated EPUB round trip and
cleanup. The last checkpoint was 19% with charging confirmed; minimum free heap
64,504 B and minimum largest block 55,284 B. This is one 30-minute trial, not an
overnight or current-consumption claim. Earlier interrupted runs are not passes.

### Physical cable removal and battery-only trials

Use `run --standby --standby-seconds 300 --cycles 2 --battery-only` with the
reader initially connected to power. The real app emits
`POCKET_STANDBY_READY <run> waitSeconds=300` after requesting standby once and
observing that the reader no longer answers its LAN probe. Ask the user to remove
the charging cable at the first marker and leave it removed through both cycles;
record that confirmation separately in the private run directory. The marker is
a coordination point, not standalone evidence that BLE/light sleep succeeded.
The usual firmware run/idle/heap/app-wake evidence remains mandatory.

Both app and runner additionally require `batteryOnlyRequested: true`,
`chargingAtCheckpoint: false` and `batteryPercent > 10`. The last checkpoint
cannot independently establish physical removal or uninterrupted battery-only
power; combine it with the user's confirmation. This neither measures current
nor changes the protective thresholds. A late/no unplug or absent power record
fails this specific acceptance even if normal Wi-Fi reconnection succeeds.

## One-shot real Wi-Fi transfer faults

`run --interface-suite --network-faults --app <checkout>` requires matching
developer firmware and the app harness. `POST /api/pocket/v1/dev/network-fault`
is Same-Wi-Fi only, requires the enrolled `deviceID`, nonzero `run`, exact fixture
`size` (128 KiB..2 MiB), and `point=receiving|published`. It rejects active uploads
and existing armed/offline/recovering trials. An unused arm expires after 60 s.
No new persistent setting, credential handling or heap allocation is needed by
the fixed trial record; the endpoint uses existing request-local JSON encoding.

Receiving triggers after a verified 64 KiB prefix, drops real Wi-Fi association
and preserves the partial stream for the normal resume path. Published triggers
only after atomic publication and durable receipt, dropping the HTTP reply.
Both disable automatic reconnect temporarily and restore its prior setting
before requesting saved-network reconnection after five seconds. Stopping the
server restores the prior reconnect setting too. All runtime hooks/routes compile
out without `ENABLE_DEV_REMOTE_FLASH`.

GET returns run/point/state/size/bytes, actual `sawOffline`, outage duration,
publication count and the latest stream's resumed offset. State values are
0 idle, 1 armed, 2 offline, 3 recovering, 4 complete, 5 expired. The runner requires
both cases, real offline observation, >=5 s outage, one publication each, resumed
prefix >=64 KiB for receiving, completed app jobs and exact downloaded bytes.
This tests association loss on X3, not a router reboot or address reassignment.
The device installation remains a separate explicit developer `--flash` action.

## Physical bidirectional reading acceptance

`--continuity-suite` invokes the sibling app's `ReaderContinuity.swift` on macOS.
It requires matching developer firmware and an enrolled X3. The runner validates
two complete bidirectional rounds, including a backward app offer, pre-acceptance
position preservation, captured page text and restoration. The Mac presses its rendered Go-to button
through local window events; it does not directly invoke the navigation method. It exports the five
run-bound X3 BMP/PNG captures from XCTest and verifies their hashes. This is HTTP
exchange plus actual EPUB/confirmation behavior, not BLE-only or iCloud evidence.

Add `--ble-continuity` (X3 `ble_standby` only) to move every position exchange onto
the bonded BLE reading-sync window. Each leg's `dev/ble-cycle?sleep=standby` run
restarts into standby, and a test-owned app link exchanges positions. The app's
normal BLE wake then returns the reader to Same Wi-Fi.
`check_ble_continuity` requires eight run-bound records (seed, unarmed control,
then app offer / post-acceptance / reader offer per round). Each needs gate
`open`, close `app-wifi`, `fromWifi`, and BLE `offers` equal to the app's sent
count (0 for the control). Armed legs need at least one list; the control needs
none. App guide: the sibling's `docs/DEVELOPER_PIPELINE.md`.

The developer-only `reader-capture` endpoint accepts `action=seed|accept|advance`
for `/Pocket visual <run>.epub` (1 byte..4 MiB on SD). `seed` records the initial page;
`accept` confirms only the reading-offer dialog after its popup is ready;
`advance` turns one real page. Completion requires a rendered page, capture,
and actual EPUB destruction/position recording before automatic Same Wi-Fi
return. GET reports the matching completion ticket; DELETE ends the fixture's
exchange visibility. Requests use the existing identity/storage admission gate.
The fixture is temporarily prepended to the normal bounded reading list without
mutating recent books. Production builds have no endpoint or automatic inputs.
The app removes the trial marker and owned book and compares the original list.
If the app is interrupted, the bounded reader timeout still returns to Wi-Fi;
resume cleanup with the saved run and recovery artifacts before starting another
trial. Removing the owned fixture also disables its exchange visibility.

No second framebuffer is allocated. The phase/action bytes are static; the
existing cold reading-list operation temporarily owns the synthetic path string
(maximum 63 bytes), rather than reserving a permanent path buffer. Existing
exchange scratch and actual reader allocations are reused.

Add `--real-epub` to use `test/reading_progress/fixtures/frankenstein-se.epub`
(226309 bytes, 38 sections). This published-book fixture retains Standard Ebooks'
original XHTML but substitutes tiny images; see its fixture README for provenance.
The runner requires its SHA-256, an unchanged app import/upload/download/reimport,
then the same two bidirectional acceptance rounds. A matching existing reader
history digest is refused; only the run-named trial is used. The 4 MiB admission
bound changes an SD-size check, not parser allocation or production behavior.

Native captures retain the user's reading orientation. The Mac runs offline OCR
with explicit quarter-turn orientations and tests each independently against the
exact source prefix; it does not concatenate candidates or accept approximate
text. Sleep verification also requires zero body-pixel difference between the
selected Brief and the actual deep-sleep frame (power/wake header/footer excluded).
Omit `--standby` for a short run; `--screen-suite` still performs its bounded timer
sleep and automatic network return.

### Isolated X4 HTTP continuity

`--x4-continuity` is a standalone Mac/X4 scenario for the generated checkpoint
EPUB. Pass a fresh private X4 config and the explicit frozen artifact matching
its installed developer version:

```sh
python3 scripts/dev_pipeline.py run --app ../pocket-daily \
  --config "$X4_CONFIG" --firmware "$FROZEN_FIRMWARE" --x4-continuity
```

It refuses flash/build-firmware/checks, other hardware scenarios, published-book
or BLE overrides, iOS and nondefault cycle options. The runner builds the Mac
host, checks the saved-address enrolled X4/STA/developer status and exact artifact
version, then invokes `ReaderX4SettingsTests/testBidirectionalReading`. It does
not fall back to the generic selected-reader discovery. Stale saved addresses
must first be refreshed through the isolated X4 discovery workflow.

The app requires `ENABLED=1`, `X4_CONTINUITY=1`, `MODEL=X4` under the hardware test
environment prefix. Its model has an empty remembered-reader store and NoRadio
transport, retains exact identity/model/version admission on every reconnect,
and leaves the selected fleet registration unchanged. Only this scenario reserves
20 following run IDs because the existing fixture uses run+20. Two HTTP rounds
and five legacy capture names remain compatible with the existing checker; outer
and continuity records identify X4, zero radio calls are required, and exported
BMP headers must be native 480x800 in addition to matching the recorded hash.

Keep supported known raw-selector, public-state and PDBF backups and
after-state comparisons outside the test. Confirm
the canonical native font baseline (14 for this acceptance fixture) and current
non-Walk prerequisites: readerActivityLoadCount=0, non-inverted output and a saved sleep
timeout longer than 90 seconds. Seed/Accept/Advance do not use Walk-only
auto-sleep suppression. The app's
reading-list/inventory and owned-file cleanup checks do not prove complete raw
state restoration. A failed non-Walk action can replace PDBF; interrupted cleanup
requires reading saved evidence before any retry. Scope is generated-EPUB HTTP
continuity; it excludes BLE, published-book coverage, iCloud and optical panel
quality. Installer, test-harness and frozen firmware provenance remain separate.

## Explicit first-install rehearsal

Developer builds expose `POST /api/pocket/v1/dev/fresh-start` only for an idle
Same-Wi-Fi session. It requires the matching `deviceID` and the literal form
acknowledgement `confirm=archive-settings-and-clear-pairing`. This is not a
shipping reset UI and is never invoked by ordinary connection or test runs.

The operation renames `/.crosspoint`, `/.pocket` and `/pocket-daily` to
`/.first-install-crosspoint`, `/.first-install-pocket` and
`/.first-install-content`, then clears only the `nimble_bond` NVS namespace and
commits it. Calibration and unrelated NVS remain intact. Root books and font
folders remain intact; history, Wi-Fi credentials, layout, cards and developer
return markers are archived. The SD backups can contain private settings and
must never be committed or shared. Existing backups reject the request before
mutation. Rename failures roll back completed moves; rollback failures and bond
errors are explicit failures, never a successful reset.

A successful response is `{"archived":true,"bondsCleared":true,"rebooting":true}`.
The reader restarts through ordinary boot with no saved-network return marker;
loss of HTTP is expected. Do not reconnect it automatically after this action:
that would consume the fresh pairing/setup state being rehearsed. A lost reply
is ambiguous; inspect the reader before retrying. This is not a full flash erase
or a claim of panel appearance based only on HTTP acknowledgement.

The X3 screen suite also captures `14-wake-transition` after the actual timer
sleep. Timer wakes enter the same splashless rendering path as a button wake;
normal boot classification is unchanged in production. The developer capture
GET/DELETE accepts `frame=wake`, with the same device/run admission and bounded
chunks. The wake capture is separate from the retained sleep BMP and is admitted
only for the current boot's recorded run. `/dev/ble-cycle` reports
`wakeCaptureRun`, `wakeRefreshMs` and `wakeSkipped`. Acceptance requires unchanged
body pixels, a changed cue, no skipped cue and 1..1500 ms for its one FAST refresh.
These are framebuffer/timing observations, not optical or physical-button proof.

For an explicitly requested repeat first-install rehearsal, an optional nonzero
uint32 `archiveRun` appends `-<run>` to all three backup names. The same run cannot
be reused if any destination exists. Earlier backups are never overwritten; an
omitted run preserves the original fixed-name safety check. Identity, explicit
acknowledgement, rollback and bond-reset rules are unchanged. This resets local
reader settings/bonds, not root books/fonts, host OS bonds or hardware calibration.

## Model-scoped short onboarding acceptance

`run --x3-only` rejects any identified model other than X3 immediately after
read-only discovery, before firmware installation or hardware scenarios. The
companion's `scripts/verify_reader_interface.py --onboarding` combines this gate
with `--wifi-setup`, interface, continuity and screen suites. It has no extended
standby or X4 stage. Failure/reading/restoration evidence is required; missing
results cannot count as an onboarding pass. OS permission dialogs and physical
first-boot button navigation are separate from injected-state regressions.

Developer `GET /api/pocket/v1/dev/reader-capture` retains `completed` and adds
`activeRun`, `pendingRun`, `fixtureRun`, and `bookReady` for the requested run.
The first three identify active RAM work and decoded on-SD capture/continuity
tickets; zero means no valid ticket, not proof that a corrupt file is absent.
`bookReady` requires the run-named EPUB to exist with an admitted size. The app
continuity harness attaches this read-only snapshot before each capture request
and includes recognized firmware rejection messages. This does not clear another
run's evidence or expose arbitrary file contents; production has no such route.

Ticket writes recreate `/.pocket` if a first-install reset archived it. Reading
acceptance must work before any screen capture: BMP capture creates its parent
directory, so running it first used to hide this missing-directory failure.
The host regression uses missing-parent write rejection and checks mkdir, open,
and short-write failures; removing directory creation makes its first write fail.

2026-10-13 acceptance: build/install run `20261013T125844-4d5717` passed 935
host tests, 135 Python tests, strict analysis and default/standby builds. Its
first reading capture succeeded before a screen capture. Final Mac/X3 run
`20261013T131358-21d2ed` on `0.1.0-dev-main-e6eb88d5-ble-standby-wdc00da92`
passed provisioning rollback, all 10 interfaces, two continuation rounds,
14 screen/sleep/wake captures and original-state restoration. The matching app
aggregate `20261013T131302-494933` passed 139 Mac tests and package validation.
X4, long standby and physical first-boot button navigation were excluded.

## X4 app settings acceptance and USB bootstrap

`--x4-settings` requires an enrolled X4 and runs the sibling app's dedicated
physical settings/preview/reboot test. Use a separate `--config` to preserve an
existing X3 enrollment. Every app preference, profile values and card bytes
are re-read; four orientation cases require exact card-preview pixels and
reboot persistence. PNG attachments are exported and their hashes checked.
The sibling `docs/X4_SETTINGS_ACCEPTANCE.md` holds the scope and actual results.

After an authorized USB install via `scripts/pio.sh run -e default -t upload`,
run `~/.platformio/penv/bin/python scripts/dev_usb_sync.py --port <USB-port>`
when that environment provides pyserial. The developer-only serial command
`CMD:DEV_SYNC_STA` verifies the existing one-byte `S` return marker, then
restarts into saved-network Sync. No credential is transmitted, no new setting
is added, and production contains no such command. Native USB open may reset
the device first. HTTP identity/version are verified again after network return.


The X4 settings suite now requires three isolated portrait EPUB changes (font
size, line spacing and margin), in addition to four-rotation settings/card checks.
The full run exports 23 native captures. `--x4-settings --x4-typography-only`
checks portrait plus those three controls after a known broad baseline; the
validator refuses to treat that reduced run as four-rotation acceptance.


### Bundled card and preset acceptance on X4

`--x4-starters --app <checkout> --config build/dev-pipeline-x4.json` runs the
app's real catalog: 36 card pixel comparisons, 12 three-card collection reboots
and seven Home/Sleep preset reboots. It rejects missing or mismatched cases,
restoration failures and stale captures, and exports/hash-checks 62 native
480×800 PNGs. The app's 36 card previews and 14 connected layout previews are
XCTest attachments. Home and Sync Brief must use verified reader snapshot
inputs and match the complete device frame before and after each reboot;
sample data cannot pass. Each card records shipping Apply/read-back/presentation
seconds so identical public cards can be compared across firmware versions.
The sibling `docs/X4_SETTINGS_ACCEPTANCE.md` describes the procedure and results.
This mode is mutually exclusive with other hardware scenarios and never flashes
implicitly. Connected layout acceptance requires `layoutInputs: 1` firmware and
its matching imported host renderer. This is input-snapshot rendering, not a
live-frame subscription or optical panel/deep-sleep measurement.

Run latency acceptance with the optional USB serial monitor closed, including
when the cable remains connected for power. This exercises normal operation
without a host draining diagnostic output. Do not open USB or issue competing
HTTP requests during the scenario; opening native USB can reset the X4. Use a
separate, explicitly labelled diagnostic run when serial evidence is needed.

`--x4-starters --x4-layout-only` skips cards/collection reboots and checks the
same seven presets (14 physical captures). Its report is explicitly focused;
it cannot pass the full-card validator or establish card latency improvement.

X4 recovery copies now also persist in the app's Application Support /
`PocketHardwareRecovery`, with device/run metadata, rather than relying on a
transient test-host tmp directory. `--x4-recover-from <directory>` selects an
existing backup explicitly. `--x4-recover-empty-revision <hash>` is restricted
to starter tests and must match the canonical empty manifest; use it only when
prior-run evidence proves the original was empty. It is not a generic reset.
Both restore through the app and verify read-back before starting acceptance.


### Wi-Fi-only developer reading walk and failure evidence

These routes require `ENABLE_DEV_REMOTE_FLASH`; production images expose neither.
Starting from production still uses the supported staged-image validation and the
reader's physical installation confirmation. A successful file upload is not an
installation, and this protocol does not bypass that first confirmation.

`GET /api/pocket/v1/dev/build-failure?deviceID=<enrolled ID>` reads only the fixed
last EPUB build-failure record. It requires idle Same Wi-Fi, matching identity,
SD availability and the render lock. The response is the validated96-byte PDBF
record (`BuildFailureLog.h`: CRC, firmware version, path hash, spine, failure
step/detail/count, free heap, largest block and uptime). No book text or arbitrary
file path is accepted; there is no clear operation. Missing records return404,
invalid global records422 and unavailable SD503. Preserve this old record before
installing or running a trial; its firmware version and hash distinguish historic
failures from the new run. Adding `run=<walk run>` selects only that run's separate
fixed failure record, with `X-Build-Failure-Run` binding the response. A walk never
replaces the user's global `/.crosspoint/last-build-error.bin`.

`POST /api/pocket/v1/dev/reader-capture?deviceID=<ID>&run=<nonzero uint32>&action=walk&pages=<1..128>`
uses only the owned `/Pocket visual <run>.epub` (1 byte..4 MiB). The host copies the
original fixture bytes unchanged to that path, verifies its bytes and records the
firmware/image identity before admission. `pages` counts completed unique page
renders **including the first page**: request21 or more to exercise20 page turns.
There is one entry reboot to leave Wi-Fi, then all turns run in the same actual
EPUB activity and boot; the reading engine, ordinary background layout, real
font/glyph cache and page renderer are used. No per-page reboot or second
framebuffer is used. Turns wait at least1 second after the preceding page. The
whole walk is bounded to600 seconds and each completed-page gap to45 seconds.
The existing activity auto-sleep guard is enabled only while Walk is active;
no user sleep setting is changed. Critical-battery and physical-power behavior
keep their existing policy. An early EOF or timeout cannot satisfy a larger requested count.

Admission writes checked run/action/page tickets and a64-byte checksummed running
receipt. Before parsing, boot validates their run/page/action agreement and arms
the existing one-shot Same Wi-Fi recovery. A missing or corrupt walk action must
not downgrade into Capture. The post-render hook counts only strictly increasing
`(spine,page)` positions after a requested turn; requests, stale `pageRendered`
flags and repeated paints do not count. A build failure after the last paint still
overrides success. Final evidence is written and read back before consuming the
recovery request; a crash returns interrupted/unknown evidence, never success.
A final owned-reader frame is saved with the existing bounded screen-capture
route, then the reader automatically reboots to saved Same Wi-Fi. If it left the
reader activity, it reports interrupted with no capture instead of capturing the
user's Home or card screen.

GET on `reader-capture` preserves existing `completed`, `activeRun`, `pendingRun`,
`fixtureRun` and `bookReady` meanings; `completed` remains the continuity ticket,
not a Walk success indicator. It adds `walkVersion:1`, `maxWalkPages:128`,
`walkReceiptPresent` (including corrupt/other-run receipts), and `walkReceiptRun`
(valid stored run, otherwise0). A fresh host run must reject any existing active,
pending, fixture or walk receipt **before uploading** its owned EPUB.
The `walk` object always includes `protocolVersion:1`, `run`, `state`,
`requestedPages`, `renderedPages`, `turns`, `lastSpine` (zero-based), `lastPage`
(one-based), `progressPercent`, `failureStep`, `failureDetail`, `elapsedMs`,
`freeHeap`, `largestBlock`, `captureReady`, `historyIsolated` and
`progressMonotonic`. A mismatched/absent run yields `state:absent,run:0`; it is not a
pass. States are `running`, `completed`, `build-failed`, `timed-out`, `interrupted`,
`end-of-book` and `capture-failed`. Only a matching completed run with
`renderedPages == requestedPages`, `turns == requestedPages - 1`, a matching final
capture and unchanged original history can pass. Heap/step/detail fields describe
the captured build failure when present; zero is not a measured heap baseline.

Walk is history-isolated even when unchanged EPUB bytes share the original
book's document digest. It does not register the continuity exchange fixture,
consume/accept offers, publish progress or destructor reading records, modify
APP_STATE/recents, or perform end-of-book moves. It may create caches only for the
owned path and write its own diagnostic files. The existing reader performance
sampler can also update the shared `/.crosspoint/reader-perf.bin`; include that
file in diagnostic backup/comparison when preserving the prior performance
record. This trial does not promise that the entire SD card is unchanged.
Capture/Seed/Accept/Advance keep
their existing behavior. The host still compares original reading history before
and after; the protocol flags describe the enforced policy, not physical proof.

The host saves result/error/frame evidence locally before successful cleanup.
Matching-run DELETE of `reader-capture` removes only its walk tickets/receipt/error
files, and refuses an active reader. A surviving checked request plus Walk action
also permits explicit cleanup of corrupt owned Walk evidence, except when the
required image diagnostics are missing, incomplete or corrupt. DELETE of screen-capture
and the owned EPUB remain separate. Failures retain their artifacts; do not retry
an ambiguous POST or erase an old run to make a new run appear clean. The static
walk controller is bounded to72 bytes by a compile-time assertion; record buffers are64/96 bytes and HTTP uses the existing staging
buffer. No new heap allocation, activity, reader setting or production route is
introduced. Hardware behavior remains unverified until the selected development
image and exact fixture run complete on the physical reader.


The Python runner prepares an offline plan without touching a reader:

```sh
python3 scripts/dev_reader_walk.py plan --config build/dev-pipeline.json \
  --expected-version '<exact-development-version>' --model X3 --pages 24 \
  --fixture en='<original-English.epub>' --fixture ko='<original-Korean.epub>' \
  --output build/dev-reader-walk-<fresh-run>
```

The authorized hardware executor uses `run --plan <plan.json> --execute
--backup-receipt <private-original-backup-receipt.json> --backup-confirmed
--no-unrecovered-transfers`. These acknowledgements require actual saved backup
and transfer evidence; a path or a local build alone is insufficient. After an
ambiguous reply or interrupted host process, `observe --plan <plan.json>
--execute --label en` observes that saved run without resending its POST.
Successful cleanup requires saved local evidence and original-history comparison;
failed runs retain both local and device artifacts. Plans, identities, addresses,
backups and captures remain private ignored artifacts.

Development `reader-capture` GET also advertises `imageDiagnosticsVersion:1` and
`currentImageRendering` (`0` display, `1` ALT placeholder, `2` suppress). The
current value is read when GET runs; it is not inferred from old settings or a
successful walk. `imageDiagnostics` is `null` when the checked record is absent,
corrupt or belongs to another run. Otherwise its fields are `version:1`, `run`,
`mode` (the setting at Walk start), `complete`, `seen`, `probeStreamFailures`,
`dimensionFailures`, `lastProbeError`, `extractAttempts`, `extractFailures`,
`lastExtractError`, `altFallbacks`, `imageBlocks`, `beforeProbeFreeHeap` and
`beforeProbeLargestBlock`. Current mode and run mode are preserved independently.

The counters cover parsing throughout the run, including background layout ahead
of the last painted page. `dimensionFailures` counts header probes that did not
obtain dimensions; it is separate from ZIP stream failure. `extractFailures`
counts the subsequent whole-image fallback ZIP extraction failures, including
failure to open its output file. The two last-error fields are captured immediately
after their corresponding ZIP call: `0` none, `1` open, `2` entry, `3` read buffer,
`4` output buffer, `5` inflate initialization/window, `6` inflate, `7` read,
`8` write, `9` method. Heap fields describe the last header probe immediately
before its ZIP allocations; zero means no observation. `imageBlocks` counts
created PageImage layout elements, not successful PNG decoding or painting.
`altFallbacks` counts emitted ALT labels; suppress mode and empty ALT do not
increment it.

Observation uses one fixed 56-byte scalar state and no per-image allocation,
framebuffer, log or SD write. The final 64-byte checksummed record and a checked
run ticket are read back before consuming the recovery request. `complete:false`
is an initial record, never terminal evidence. Missing, partial, corrupt or
incomplete diagnostics preserve the request and block cleanup; orphan image
records also block a new run. The existing global build-failure record is never
changed. JSON uses at most 2048 bytes of the existing 4096-byte staging buffer
and rejects overflow instead of sending truncated JSON. All device observation,
storage and endpoint additions compile out of production.

Development GET additionally reports `currentReaderFontPoints` from the current
native `fontPointSize` setting, not the companion's coarser font-size bucket.
It is read-only and does not describe a previous run or change the setting.

`paintDiagnosticsVersion:1` advertises a separate `paintDiagnostics` object;
`null` means absent, corrupt or another run. Its fields are `version:1`, `run`,
`complete`, `renderCalls`, `cacheHits`, `lazyExtractAttempts`,
`lazyExtractFailures`, `lastLazyExtractError`, `decoderAttempts`,
`decoderSuccesses`, `placeholderCalls`, `suppressedCalls`, `pngAttempts`,
`pngEntryFreeHeap`, `pngEntryLargestBlock`, `pngObjectBytes`, `pngHeapGateBytes`,
`firstFailureStage`, `firstFailureCode`, `firstFailureFreeHeap` and
`firstFailureLargestBlock`. Counts saturate at uint32 max and include repeated
BW/grayscale passes; prewarm scans are skipped. Lazy extraction is the render-time
ZIP call, distinct from the layout fallback extraction in image diagnostics v1.
Its ZIP error is cleared before and captured immediately after that call, so a
wrapper failure before entering ZIP reports0 rather than a stale earlier error.
PNG entry memory, actual
`sizeof(PNG)` and the heap gate are captured only on the first paint decode;
`pngAttempts:0` leaves all four entry fields zero. Entry free heap is the exact
value used by the existing PNG gate; new largest-block and failure measurements
use `HalMemory::getDefaultHeap()`. These are distinct from the
heap measured immediately at the first failure, which stays latched through
later suppression, decoder failures and successful passes.

Failure stages are `0` none, `1` lazy ZIP extraction, `2` image file open,
`3` empty image, `4` decoder factory, `5` PNG heap gate, `6` PNG allocation,
`7` PNG open, `8` PNG dimensions, `9` PNG crop, `10` PNG internal row bound,
`11` PNG bit depth, `12` PNG expanded gray row bound, `13` PNG line allocation,
`14` PNG decode, `15` generic decoder result and `16` render bounds.
`firstFailureCode` is signed int32: the ZIP error for stage1, PNG return code for
stage7/14. Stage6 now identifies the failed allocation/binding component:
`1` PNG object, `2` 32 KiB dictionary, `3` inflate state, `4` scanlines,
`5` workspace binding; older images reported0 for the embedded decoder object.
Other stages use0. Stage0 has zero code and failure heap fields. The diagnostic
record and stage numbers are unchanged; observations alone do not verify pixels.

PNGdec remains pinned to 1.1.6. `scripts/patch_pngdec.py` applies the reviewed
three-file patch only to the active project-local PlatformIO environment, checks
upstream/patched file hashes and the patch checksum, and refuses missing, changed
or partially patched sources. Clean builds compile every PNG translation unit
against the same patched header. Native callers can apply the same patch with
`python3 scripts/patch_pngdec.py --library <local-copy-of-PNGdec>`.
The reproducible native API regression (no downloads) is
`python3 scripts/test_png_workspace_native.py --library <patched-local-copy> --output build/png-workspace-api`;
it checks a nonuniform pixel oracle, invalid regions, repeated decode, close,
reopen and the unsupported acceleration mode. `scripts/test_patch_pngdec.py`
checks patch provenance, idempotency and active-environment scope.
The decoder algorithm, 32 KiB dictionary and existing row/bit-depth limits remain.
Caller-owned transient allocations replace the embedded 59,456-byte ESP object:
32 KiB dictionary, `sizeof(inflate_state)` (7,128 bytes on ESP32-C3), and exactly
`2*(pitch+1)+32` scanline bytes (at most 16,416), plus at most 15 bytes of alignment
padding. The smaller PNG object retains palette, metadata and file buffering;
`pngObjectBytes` reports this object's size, **not** the total workspace.
The existing 61,440-byte total-heap admission gate is unchanged; allocation can
still fail on a fragmented heap. Workspace storage is fallible and temporary,
bound only after successful header parsing and validation, and outlives close
and decoder destruction. Null, short, overlapping, overflowing or unaligned
regions are rejected; open/reopen and close discard bindings. Binding restores
the original zero initialization. Repeated decode remains supported.
`PNG_FAST_PALETTE` is rejected explicitly because this firmware never uses its
extra row-array alias; the normal option 0 path and CRC option retain their
existing behavior. No cache-format change or automatic cache deletion is added.

Cold reader images are prepared before the page font scan/prewarm. The renderer
first releases the preceding RAM pixel-cache slot and clears rebuildable glyph
caches while preserving layout advance tables, then renders only image elements
that still need decoding. Successful decoding writes the existing SD pixel cache;
the normal text prewarm, BW refresh and grayscale passes follow unchanged. Cached
images are excluded from preparation so their RAM slot cannot crowd a later cold
decoder. No framebuffer, decoder limit, cache format or failure-suppression lifetime
changes. A decode without a valid persisted pixel cache still follows the normal
BW fallback; successful preparation alone is not proof of final pixels.

Paint observation occupies at most 96 bytes of fixed state; its checked SD record
is 88 bytes. Hooks add no heap allocation, framebuffer, log or SD write. Boot initialization and
terminal walk handling write and verify the run-bound record. New walks store
a distinct checked action5 ticket before boot (normalized to ordinary Walk in
RAM), so missing paint files cannot be mistaken for legacy completed walks.
An orphan paint marker/record blocks every new reader trial. New walk admission
writes the paint marker first; a torn later write preserves that unknown state.
New walk cleanup/recovery requires matching complete paint evidence, including
when both paint files are missing but action5 survives. Existing action4 walks
without paint files retain firmware-side cleanup compatibility. The newer Python
runner still fails closed if new firmware advertises paint support but that old
result has no paint record: finish and clean trials before upgrading; automatic
resume/cleanup of a pre-upgrade trial is not supported. Non-walk Capture,
Seed, Accept and Advance do not require a fresh paint record. Existing image-v1
and global build-failure records retain their meaning. All new observation,
storage and endpoint code is under `ENABLE_DEV_REMOTE_FLASH`.

The Python runner saves the complete terminal `reader-capture` response as private
`terminal-readiness.json` before cleanup, with its byte count and SHA256 in the
report. Image and paint diagnostics, the current `currentReaderFontPoints`
setting snapshot and unknown future fields are kept. When either diagnostic
capability is advertised, a missing, incomplete, malformed or wrong-run record
stops cleanup after preserving the response, PDBF, available capture and public
state. Older firmware without these capabilities remains compatible. Finish and
clean up a pending trial before upgrading firmware: an old pending trial cannot
be automatically observed or cleaned up under a new version, and a newly
advertised capability never makes a missing diagnostic record acceptable.
`imageValidation` and `paintValidation` are always `not-performed`; both
verification-pending flags remain true. Image-block counts include layout-ahead
work, and paint counters describe renderer attempts rather than verified pixels.
Neither an ALT fallback, decoder-success count nor a successful page walk proves
image appearance or geometry. The current font snapshot is not a full settings
backup or proof of restoration.

The shared Python client used by `dev_reader_walk.py` and `dev_library_walk.py`
retries only a GET expecting 200, with no request body, on these exact complete
409 responses: `dev/reader-capture` → `Reader is drawing`, or `files/content` →
`Reader is drawing; retry the download`. It allows at most six attempts with
one-second backoff under a monotonic ten-second request budget. Remaining budget
reduces each request timeout and is checked before another attempt and successful
return; it is cooperative, not a separate watchdog that cancels an in-flight
socket read. The response/socket closes before backoff. Existing longer trial
observation deadlines are unchanged.

POST, PUT and DELETE remain single-shot. Other statuses/bodies, incomplete error
bodies, transport errors and redirects do not trigger this retry. Errors retain
only the method, a fixed route, status, attempt count and an allowlisted fixed
body; at most 257 error bytes are read and unknown/truncated bodies are redacted.
Host, query, identity and filenames are excluded. This adds no generic mutation
recovery: preserve failed reports and state after ambiguous outcomes, and never
replay an unknown POST or commit merely because a later GET was busy.

## Developer Library entry trial

`dev_library_walk.py` is a separate, explicitly opted-in developer experiment.
It requires an already installed exact developer build, enrolled device ID,
Same Wi-Fi, a fresh diagnostic state and one unchanged EPUB fixture. It does
not install firmware. Production builds omit its device endpoints and hooks.
The normal Capture/Seed/Accept/Advance and Walk action values remain unchanged;
Library uses persisted action 6 and requires the existing image and paint
records. Finish and clean any earlier trial before changing firmware versions.

The reference is an exact ASCII root EPUB path whose bytes and size equal the
local fixture. The temporary `/Pocket visual <run>.epub` copy has its own cold
path-keyed cache. The trial enters the real `LibraryListActivity`, builds or
opens the normal index, locates and captures the reference row without opening
it, captures the temporary copy's row, then calls the existing
`openSelectedBook()` once for that copy. Both rows must have the expected
metadata title and be fully inside the actual rendered viewport; merely being
in the loaded row window is insufficient. The subsequent reader walk retains
its history isolation. Neither row counters nor a successful page walk proves
visible title glyphs, image pixels, buttons or optical panel quality: review
the two saved Library BMPs and reader BMP independently.

Before upload, the runner privately saves the exact known `state`, `recent`,
`settings`, `index` and `dirty` files through the fixed `libraryBackup` selector.
There is no arbitrary-path read or write API. Each chunk is run/offset/size
bound, each full file is SHA-256 checked against a manifest, and a second
manifest must match. The upload and admission use one-shot requests; original
book bytes, state, recents and settings are checked again before opening and
before cleanup. Existing unfinished index staging, noncanonical recents,
unknown diagnostic ownership and any competing upload reject admission.
Normal recent pruning/persistence is suppressed only for this isolated trial.

The derivative Library index may be rebuilt. It is backed up for inspection,
not restored over a newer index. Successful cleanup removes only the owned
book/cache, marks the index dirty, rebuilds it through the normal builder, and
checks that the owned row is absent while the reference row remains. State,
recents and settings must retain their original bytes, and the public reading,
preferences, profile, content and original build-failure snapshots must match.
This is not a complete SD backup or a claim that every original cache byte was
examined. The original book is never opened or its cache explicitly removed.

The developer builder budget caps 4,096 scanned entries, 131,072 work units and
45 seconds of cooperative work. It checks before/after metadata extraction and
before index replacement or clearing an unchanged index's dirty marker.
Individual SD, metadata and sort calls are not preempted; this is not a hard
45-second response guarantee. A failed budget retains the previous index.
The Library preparation also has a cooperative 90-second bound; the subsequent
reader walk keeps its existing page and time bounds.

A checked Library record and run marker survive interrupted admission, reboot,
frame failure and partial cleanup. The runner saves the complete terminal JSON,
all available run-bound frames, build-failure evidence and before/after raw and
public state before accepting completion or deleting anything. An interrupted
Library phase may coexist with a still-running saved Walk record; this is a
failed trial, not permission to replay the open. Failed or ambiguous runs retain
the owned book/cache and diagnostic records for inspection. The runner does
not provide generic remote recovery or automatically erase those failures.

Successful cleanup is one `DELETE reader-capture`; a lost reply is observed
without repeating it. A distinct `libraryAck=true` DELETE removes the successful
run's evidence only after all three frames, protected state checks, final
index and original-byte verification are durable locally. A failed or lost ACK
is never replayed automatically. Both deletion gates recheck matching request,
action, page, Walk, image and paint ownership. Do not start another trial until
any retained or partially removed evidence has been reviewed.

Prepare with `python3 scripts/dev_library_walk.py plan --help`; planning reads
only local configuration and freezes unchanged fixture bytes. `run` requires
`--execute --backup-confirmed --no-unrecovered-transfers`; `observe --execute`
can resume only the same authorized pending one-shot admission, not a cleanup
or ACK. All backups, book paths and raw settings remain in mode-0600 files
inside the private plan directory and are not printed to stdout.

Offline validation covers checked-record corruption, request ownership,
fully-drawn row admission, one-shot opening, actual LibraryBuilder/LibraryIndex
path/title/order and cleanup behavior, and injected Python transport/state
failures. The index tests use deterministic metadata fixtures; they do not
execute the full Activity/widget lifecycle. Actual Library frames and opening
on X3/X4 remain separate device acceptance gates.

## Image-bound production trial lease V4

`scripts/dev_trial_lease.py` is an explicitly authorized developer acceptance
workflow, separate from the shipping app's firmware-staging/reader-confirmation
flow. Plan offline first; each stage/start/cleanup/clear uses a fresh private
output directory and requires `--execute`. Inspect the command's `--help` and
prior receipt before any mutation. Never replay a lost POST/DELETE or reuse a
consumed intent. Preserve the original active image, whole known book bytes,
public state, fixed raw files and known caches before installation.

The V4 CRC-protected NVS record binds model layout, run, production image length
and SHA-256, validated return slot and bounded duration. Its capability is
`POCKET_TRIAL_LEASE_V4`; older trial records are rejected. The production status
contains an optional `trialLease` witness with `version: 4`, run, image hash,
seconds, slot and surface. Optional root `largestFreeBlock` and
`minimumFreeHeap` are observations, not shipping capabilities or authentication.
The app's NearbySyncProtocolTests verifies that these extra fields do not grant
new shipping features.

Explicit surfaces select existing Sync, File Transfer STA, Home, Library or
Settings activities. Sync/File Transfer may run at most900 seconds; UI surfaces
at most180 seconds. The normal six-field Sync producer still caps180 seconds;
reader-auto-wake remains360 seconds. Home/Library/Settings capture only after
the ordinary render completes, save a run-owned BMP and memory checkpoint, then
return. The provisioned image-bound route is not a production remote-control or
flash endpoint; ordinary boots and reader installation confirmation are unchanged.
The timer callback only records a checked deadline ticket and restarts; return
slot/hash validation runs in the main task before SD operations.

The optional reader fixture completes20 native pages, captures the image after
initial indexing and cache reopen, observes an owned-file absence/recovery, and
samples HAL/ReaderPerf reading minima. Auto-wake releases the normal one-minute
inactivity clock and requires actual timer reset/wake, matching persisted
position and a completed next page. It does not establish mechanical power,
battery cold-on, physical SD removal, optical display quality or battery drain.
The UI memory checkpoint and pre/ready HTTP checkpoint are stage-specific.
Home/reading retain the >50KiB criterion; radio gates remain independently
specified in `release-checklist.md`.

For File Transfer, `dev_trial_transfer.py` freezes exact1MiB/20MiB valid EPUBs,
a4097-byte boundary TXT and a4099-byte Korean-name TXT. Acceptance compares
whole-response bytes/SHA, framing and HTTP1.0, slow reads, RST and deliberate
stalls with no transport retries. A subsequent Sync `--download-plan` must bind
the same reader/image and the immutable File Transfer owner plan. It checks
public4096-byte piece responses, whole reconstructed SHA and malformed/forbidden
requests. Only a complete recognized low-memory503 GET body gets bounded
backoff; ambiguous transport failures and mutations never retry. Cleanup hashes
all owned fixture bytes before one deletion each; it never removes user books.

R13 `build/release-closeout-20261028` is the private orchestration/evidence folder.
Its frozen runner additionally checks the existing eight preferences across all
four orientations, rejects mixed-invalid changes atomically, restores the prior
values and round-trips a generated Unicode transfer. After app tests the owner
restores the original active firmware and nine known fixed SD files (including
ReaderPerf bin/tmp), clears owned trial/cache files and compares original public
state and whole books. This is scoped restoration, not whole-SD/NVS/flash equality.
Failed trials and unknown outcomes must retain their evidence and remain failures.

For an actual-app public-interface run, `plan --app-acceptance --seconds 600`
uses a separate explicit Sync lease (maximum900 seconds). It cannot be combined
with reader, auto-wake, UI/File Transfer or owned-download-plan scenarios. The
plain/default Sync producer retains180 seconds. Its host-only plan bit sends the
existing V4 explicit `surface: sync` payload; no firmware endpoint or image change
is involved. Check the production run/image witness before launching the test
host; do not run the normal polling/download observer concurrently with it.

Use the app's explicit `testProductionConnectionAndInterface` opt-in, separate
from developer flags. Capture its original-state/recovery attachments and exact
run-bound result; this executes shipping model calls, not physical button UI.
Before its profile roundtrip, additionally back up both fixed
`/.crosspoint/pocket-profile.{0,1}.bin` slots through the bridge's File Transfer
mode. After the bounded production return, clear only the matching terminal
lease, restore those slots once with whole readback, then restart Sync and compare
both original profile values and generation. The public profile API cannot roll
back generations. Keep these two extra slot receipts alongside the existing
nine-file restoration, and never claim whole-SD or whole-NVS equality.

### Build-storage retention

Reuse the native PlatformIO/host build and versioned package cache unless a
compiler/SDK comparison requires isolation. Archive each unique accepted image
and its ELF/MAP/source pins once; reference it from hardware plans. A staging
backup needs the specific hashed return image and metadata, not a recursive copy
of the entire historical `firmware/` directory. Verified APFS clones retain
independent future writes without duplicating identical historical file blocks.
Keep original-device backups, exact accepted candidates and failure receipts.
Do not remove those as if they were compiler caches. The 2026-10-29 audit removed
13.01GiB of old app compiler caches and converted3270 identical old package/BIN
files to verified clones; actual available space increased16.61GiB.

The plain Sync `--transfer` receipt records its owned destination as `target`;
reader/FT plan entries use `fixture`/`path`. Normalize the receipt only after
checking `target == plan.fixture`, then verify its complete bytes/SHA before one
DELETE. Do not assume every receipt uses a `path` key, or replay an ambiguous
cleanup mutation. R13 retains its original failed helper and corrected mapping.
