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

No IP input or re-enrollment is needed for a network/DHCP address change. Reports
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
The Mac app is built before preflight so it can discover an unreachable saved IP.
All requested firmware/app build and test gates precede installation. An exact
version already installed skips flashing. Otherwise upload uses the existing
port-82 acknowledgement/commit protocol, sends the developer flash request once,
and verifies the same reader and exact embedded build version after reboot.
Neither an ambiguous installation nor a standby request is blindly replayed.

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
1–65536 bytes, and writes a checked request plus one-shot `R` boot marker. Before
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
