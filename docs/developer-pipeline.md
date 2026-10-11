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

From the firmware checkout:

```sh
python3 scripts/dev_pipeline.py configure --host <reader-ip>
python3 scripts/dev_pipeline.py run
```

Enrollment stores only the LAN address and expected reader identity in ignored
`build/dev-pipeline.json`, mode 0600. Runs reuse it; a different reader is rejected.
Re-enroll deliberately if DHCP changes the address. The runner never switches
the Mac/iPhone to the reader's private hotspot. An offline reader at preflight is
a failure; the current pipeline starts in Same Wi-Fi and tests sleep inside its
controlled scenario. It does not bootstrap release firmware or recover a reader
that cannot boot/join Wi-Fi.

## Iterate

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
confirmation. The image is frozen under the run directory before app builds.
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
