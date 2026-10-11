# BLE standby feasibility — 2026-10-09

Latest installed follow-up (2026-10-09 23:04 KST):
`8925b142-ble-standby-w79b237a9` preserves GPIO configuration during automatic
light sleep. The inherited SDK enables `CONFIG_PM_SLP_DISABLE_GPIO`, conflicting
with standby keeping the SD card mounted (GPIO13 supply, chip selects) and
polling buttons. The HAL applies IDF's documented `gpio_sleep_sel_dis` override
to valid pins before enabling standby; failure refuses standby. Terminal deep
sleep still explicitly shuts down rails. See `developer-pipeline.md` for the
SDK source and bounded, SD-persisted developer checkpoints.

Before the fix, an actual Mac/X3 wake trial failed at BLE connecting and did not
return on its 180 s recovery deadline. Manual recovery exposed an Interrupted
record, without intermediate details. This is distinct from a post-boot
low-memory WAKE gate. After the fix, two 90 s app trials passed: actual standby
83,058 / 82,120 ms, light sleep 66,893 / 65,988 ms, close `app-wifi`, app LAN
return 20.61 / 13.12 s, two completed READ_LISTs each, and a generated 2,856 B
EPUB upload/download byte comparison plus cleanup each. Battery was 99%; minimum
free heap 61,280 B and largest block 53,236 B. No current measurement, hours-long
standby, physical iPhone or X4 result is implied.

Artifact: 6,407,920 B, SHA-256
`6588fb6ca7de031f06132f2e243976fae76697993973d7cee246bab7aac90f75`.
Local evidence: `build/dev-pipeline-runs/20261009T135746-4d33d3/report.json`,
with a stricter duration revalidation requiring >=60 s actual standby.
Host tests 898/898, pipeline Python tests 22/22, strict cppcheck and warning-free
default/standby builds passed. Removing checkpoint persistence failed the new
interrupted-trial regression. Earlier artifact identities below are historical.

## App-requested wake implementation (latest, experimental X3)

The `ble_standby` environment now implements retained-screen BLE standby and
the companion's explicit Connect → authenticated `START_WIFI` → saved-network
handoff. Build with `scripts/pio_ble_standby.sh`; its SDK packages are cloned
under ignored `build/ble-standby/packages` so the PM configuration cannot silently
replace the default/release SDK. Resolved ESP-IDF 5.5.5 headers enable PM,
tickless idle, controller modem sleep and main-crystal retention during automatic
light sleep. No unverified external 32 kHz crystal is assumed.

The sleep frame is painted and released once. Display sleeps, SD remains mounted,
the full-speed lock is released and the existing authenticated BLE worker runs
with 1 s advertising and a 25 ms watchdog-fed owner loop. SDK automatic light
sleep owns radio wake scheduling. `WAKE1` is advertised only in standby;
`START_WIFI` is accepted only there from an authenticated bonded peer. A normal
reading exchange leaves standby active. A button returns to reading. Battery
is checked every minute; failed admission or battery <=10% falls back to deep
sleep. X4 standby is gated off.

An accepted app request is acknowledged before BLE shutdown, then a deliberate
software restart rebuilds rendering and joins saved Wi-Fi. A checked one-shot
RTC ticket distinguishes this route from button return and ignores cold/crash
boots. This uses the terminal framebuffer lifecycle; it does not claim same-boot
restoration or hide accumulated fragmentation. There is no default timer poll,
automatic AP, or change to the Mac's network. Ordinary standby has no window
deadline; developer trials alone have a 180 s recovery deadline.

Installed candidate: `0.1.0-dev-fix-ble-window-cache-preflight-8925b142-ble-standby-w9ef85a63`,
6,406,608 B, SHA-256
`9257fa419be56f31be4f25e06a7635dc1dd3e98337814f7c39316be59e6b53ce`.
The first X3/Mac trial used the actual app UI and completed without reader
interaction: BLE wake → joining Wi-Fi → connected. Firmware recorded close
`app-wifi`, 611 actual light-sleep exits and 13,888 ms asleep during 17,607 ms
standby, with min free heap 64,564 B / largest block 57,332 B. It reclaimed
52,272 B before radio startup and completed one quiet READ_LIST. SDK sleep
callbacks measure time spent asleep, not current or battery life. Preferences
returned HTTP 200 after reconnect; inventory returned a separate folder-read
error, so content-transfer success is not claimed from this trial.

Validation: 886/886 host tests, 10 pipeline tests, strict cppcheck 2.11 and a
warning-free default firmware build passed, as did the isolated experimental
SDK/firmware build. A mutation restoring the ordinary OPEN deadline made the
standby regression fail. The app transport/protocol suite passed 134 tests;
the signed Mac app was installed with its existing sandbox and pairing intact.
No X4, physical iPhone, all-orientation/button visual acceptance, prolonged
battery test or current measurement is implied. Default/release builds still
use the bounded behavior below. Historical sections describe preceding stages.

### Final Wi-Fi-to-standby candidate

Source review after the first three successful shell-entry trials found that
sleep from a Wi-Fi activity could skip BLE: its radio was still enabled when
the sleep path checked ownership. Experimental `onExit()` now performs normal
server/BLE/DNS teardown when the sleep latch suppresses its restart, then the
sleep path stops the Wi-Fi driver before BLE admission. Default behavior is
unchanged. The developer trial now joins saved Wi-Fi before sleeping and its
acceptance requires durable `fromWifi=true`, so the shell-only trial cannot
satisfy this gate.

Final candidate `0.1.0-dev-fix-ble-window-cache-preflight-8925b142-ble-standby-wd4168ca4`
was remotely installed and its reported version checked: 6,406,832 B, SHA-256
`4d3ed535dd786c61da638ca7eef5fce61888df836fc5952539b97b5bbf1e0c0d`.
First Wi-Fi-entry trial passed: `fromWifi=true`, close `app-wifi`, 407 actual
light-sleep exits / 8,840 ms asleep over 11,229 ms standby, minimum BLE free heap
64,016 B / largest block 53,236 B, and same-identity STA return. The actual Mac
app displayed connected. This foreground-only request did not perform READ_LIST
or OFFER; those are distinct from authenticated wake acceptance.

The updated host suite passes 887 tests; Python cycle/upload/route suites pass
49 tests. Local evidence is in ignored `build/ble-standby/wifi-entry-*`.
The second final-image trial also passed from connected Wi-Fi: 80,759 ms standby,
2,941 actual sleep exits / 64,823 ms asleep, one quiet READ_LIST followed by the
explicit app wake, minimum free heap 60,936 B / largest block 53,236 B. The final
signed Mac app was restarted during standby; one Connect Reader click completed
the wake and automatic LAN connection. Across both images, five app-wake trials
passed; only the last two verify the final image and Wi-Fi-entry path.
Final strict cppcheck, warning-free default build, experimental build, iOS build
and signed Mac build passed. The original default artifact staged by the last
verification build is not the installed experimental image; the exact installed
candidate is preserved at ignored `build/ble-standby/wifi-entry-candidate.bin`.

## Evidence and limits

The available reader is an X3 reporting version `0.1.0`. Its read-only status
returned a retained wake-window attempt with `lastGate=low-memory`, 80,100 bytes
free and a 61,428-byte largest block, and no opened window. These RTC statistics
survive restart: they identify an observed attempt, not its exact age or commit.
The active Wi-Fi session's 32,344-byte free heap is a different measurement and
must not be used as the BLE startup budget. Raw status stays in ignored build/.
No X4, current meter, or power analyzer is available. No sleep current or battery
life improvement has been measured. The version string alone cannot identify the
installed commit. The Mac must retain its normal Wi-Fi during BLE testing.

The inspected local default build uses ESP-IDF v5.5.5. Its resolved SDK header
has no enabled `CONFIG_PM_ENABLE`, `CONFIG_FREERTOS_USE_TICKLESS_IDLE`, or
`CONFIG_BT_CTRL_MODEM_SLEEP` (compiler preprocessor probe, not only ini text).
This is evidence about the local build, not proof of the installed image's SDK.

## Implemented improvement

Manual network startup releases rebuildable font caches, while automatic
reading-sync startup previously evaluated the retained shell/sleep caches as
unavailable memory. `ExchangeWindowMemory.cpp` now retries only a LOW_MEMORY
admission after `FontCacheManager::releaseSdFontCaches()` under `RenderLock`.
It re-samples total and contiguous heap after reclamation. Font families remain
loaded; caches rebuild on the next paint. No new heap allocation, persistent
buffer, setting, radio mode, or protocol field is introduced.

Disabled/unbonded/low-battery windows never touch caches. Windows already meeting
the budget avoid cache churn. Startup (96/40 KiB), ready (24/8 KiB), and running
(20/4 KiB) free/block floors remain unchanged. `readSync.startFree/startBlock`
now describe the final admission sample, after reclamation when attempted.
No assertion is made that these caches account for the entire observed deficit
or that this X3 will now pass admission. A later repaint can rebuild them while
BLE is active; existing running guards remain necessary.

## Why always-available BLE is a separate change

Deep sleep disables the radio. A retained e-ink frame alone does not indicate
whether the CPU/radio is asleep. ESP32-C3 supports BLE modem sleep with automatic
light sleep, but enabling it is not a replacement of one sleep API:

- Rebuild the SDK with power management, tickless idle and BLE modem sleep.
- Select a supported BLE sleep clock. The installed IDF C3 power-save example
  provides a main-crystal configuration, so an external 32 kHz crystal is not
  an absolute requirement. X3/X4 external-clock availability is unverified.
  Keeping the main crystal on has an energy cost. Do not select the internal
  136 kHz RC clock for connected BLE.
- Integrate ESP-IDF PM locks with `HalPowerManager`'s manual CPU frequency changes.
  Current BLE windows force normal CPU speed and poll every 10 ms; the sleep
  preparation also holds a normal-speed lock. Merely extending the window is
  not a low-power implementation.
- Bound wake latency, handle buttons and SD/display power, and deinitialize BLE
  before reading or Wi-Fi. Keep authenticated bonded-only reading exchange;
  a remote request must not silently enter the hotspot path.

Sources:
- [ESP32-C3 sleep modes](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-reference/system/sleep_modes.html#wi-fi-bluetooth-and-sleep-modes)
- [BLE sleep clock requirements](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-guides/low-power-mode/low-power-mode-ble.html)
- Local installed IDF: `examples/bluetooth/nimble/power_save/sdkconfig.40m.esp32c3`
  and `components/bt/controller/esp32c3/Kconfig.in`.

## Structural solution review — 2026-10-09

Recommendation: give retained-screen BLE standby its own resource and power
lifecycle. Rendering and radio startup currently overlap in memory even after
the last useful frame is on the panel. Fix that ownership overlap, then enable
controller modem sleep plus automatic CPU light sleep. These are two separate
problems: opening a window does not establish low-power standby.

The one-way pre-sleep resource release below is now implemented and has X3
memory/exchange evidence. Reversible connected standby and SDK low-power
integration remain design work. No current measurement is available.

### Where memory is retained

| Resource | Current evidence | Consequence |
| --- | --- | --- |
| Primary framebuffer | `FreeInkDisplay::allocFrameBufferStorage()` allocates the runtime panel size: X3 52,272 B, X4 48,000 B | The physical image remains on e-ink without keeping its RAM copy. This is the largest bounded reclaimable owner. |
| Rendering task | `ActivityManager::begin()` allocates a 12,288 B C3 stack | Blocking/suspending the task does not free its stack. Reclaiming it requires a cooperative stop with acknowledgement. |
| Pocket Daily activity | Target ELF DWARF: `sizeof(PocketDailyActivity) == 5,256` | `enterDeepSleep()` preserves the activity when it owns the sleep frame; member-owned allocations are additional. |
| Base settings descriptions | Target ELF: `sizeof(SettingInfo) == 132`; preprocessed C3 initializer has 71 entries, or 9,372 B of vector elements | The function-local static in `getBaseSettingsList()` lives for the entire boot, even outside Settings. Enum label allocations and allocator overhead are additional. |

The settings entry count is the initial capacity: runtime filtering erases
entries without shrinking it. Compiler probe files and accounting are in
ignored `build/ble-architecture-review/`. They used the default target flags,
not the host ABI. The same probe reports an 11,696 B stack frame for the base
list constructor. Simply changing the static list to a temporary would rerun
this constructor in request/save paths; it is not the recommended fix.

The latest retained admission sample was 85,824 B versus a 98,304 B floor:
a 12,480 B deficit. Releasing the X3 framebuffer alone has a 52,272 B payload,
more than four times that deficit. Holding all other allocations equal gives
138,096 B before BLE startup. This is budget arithmetic, not an observed
post-release heap value, largest-block result, or proof that an exchange fits.
The sleep-trigger baseline must be measured separately from this wake sample.

Relevant ownership points:

- `src/main.cpp:283`: `enterDeepSleep()` paints/saves the retained frame, then
  runs BLE before display/storage shutdown. The full-speed power lock spans
  that whole operation.
- `src/main.cpp:345`: `setupDisplayAndFonts()` starts display, renderer, render
  task and fonts together; there is no complementary quiesce/resume lifecycle.
- `src/activities/ActivityManager.cpp:70`: a render task can consume a pending
  notification. A sleep flag or a null framebuffer alone cannot stop it safely.
- `lib/GfxRenderer/GfxRenderer.cpp:386`: the existing framebuffer loan lends
  bytes **in place** to build scratch. It does not return memory to the heap,
  so NimBLE's ordinary allocator cannot benefit from it.
- `freeink-sdk/libs/display/FreeInkDisplay/include/FreeInkDisplay.h:319`:
  `releaseBuffers()`/`reallocBuffers()` already exist, but the firmware HAL
  exposes only the in-place loan. Renderer and SDK both hold framebuffer
  pointers; a release must invalidate both after rendering/refresh completes.

### Smallest useful structural change

Implemented: a **one-way, pre-deep-sleep headless exchange**:

1. Stop an existing BLE window before painting, then paint and save the final
   sleep frame while the display and SD are available.
2. Acquire `RenderLock`, wait for an in-flight panel refresh, and release the
   framebuffer through HAL/GfxRenderer. A queued render observes the missing
   framebuffer under that same lock and skips drawing while still acknowledging
   its waiter. The task and font families stay allocated; neither is deleted.
3. Through HAL/GfxRenderer, release the actual framebuffer and clear cached
   pointers. Preserve the panel image. Keep storage available for READ_LIST
   and OFFER; they use SD and cannot run after `Storage.prepareForDeepSleep()`.
4. Re-sample heap/block, keep all admission floors, run the existing bounded
   bonded-only exchange, and fully deinitialize NimBLE.
5. Finish ordinary display/storage sleep and the board-specific power-down.
   This path never reallocates the framebuffer in the same boot.

This is a bounded first implementation because there is no display restoration
on that terminal path. It addresses the pre-sleep memory overlap only; it does
not by itself fix wake/book-close windows or make the device remotely reachable
after deep sleep. It must not be presented as completion of connected standby.

### Complete connected-standby lifecycle

Use explicit Reading, Quiescing, BleStandby, Exchanging, Restoring and Off
states with a single owner of render/radio transitions:

- Reading -> Quiescing: finish the frame, persist reading position and stop
  all users of render-owned buffers. Establish the resource boundary above.
- BleStandby: keep CPU/radio power available, Wi-Fi off, display asleep and
  rendering quiescent. Use bonded service advertising and automatic light
  sleep between radio events. A phone request wakes work on the owning task;
  it need not refresh the screen or load the reader UI.
- Exchanging -> BleStandby: perform the existing bounded reading exchange,
  release its transient work, then return to idle. Initially keep SD mounted
  for correctness. Gating the X3 SD rail requires a separate HAL suspend/resume
  contract that closes all files, restores power and remounts before access;
  the current setup-only `Storage.begin()` is not that contract.
- Button -> Restoring -> Reading: stop BLE callbacks/worker, deinitialize the
  controller, release all standby-owned allocations, then allocate the primary
  framebuffer **first**, rebind renderer pointers and restore fonts/activity.
  Allocate/check everything required for reading before accepting more work.
- Battery/policy -> Off: use the existing board-specific terminal sleep path.
  BLE reachability ends in this state.

The SDK explicitly records progressive fragmentation from past framebuffer
free/reallocate cycles. Therefore the reversible lifecycle must prevent any
standby allocation from surviving into Restoring, and must demonstrate stable
largest blocks over repeated cycles. Reallocating first reduces competing
allocations; it is not a proof of recovery. Do not merge the reversible path
without an allocation-failure recovery design and repeated device evidence.
Do not add automatic reboots to conceal a shrinking heap.

Settings are a separate baseline improvement: move fixed descriptors/labels
into flash-backed immutable tables and materialize dynamic UI choices only
while their screen/request exists. Persistence should consume the immutable
schema directly instead of constructing `getSettingsList()`. Preserve numeric
enum values, JSON keys and runtime font setters. Removing its 9,372 B element
payload alone is smaller than the observed deficit, so this is not a substitute
for the standby resource boundary.

### Automated developer validation

`scripts/dev_ble_cycle.py` preserves the Mac network and verifies the reader's
identity and exact installed version before and after each run. With `--build`
it runs the full host suite, route/upload/pipeline Python tests, strict cppcheck
2.11 and the default firmware build before staging and explicit remote flashing.
An ambiguous flash or cycle POST is never repeated. Optional bounded private-LAN
discovery finds the same reader if its IP changes; changed-IP recovery has host
checks, not a forced DHCP-change hardware trial.

```sh
python3 scripts/dev_ble_cycle.py --host <reader-ip> --build \
  --cycles 3 --require-exchange --timer-sleep
```

The developer-only `POST /api/pocket/v1/dev/ble-cycle?run=<uint32>` persists a
verified one-shot request, then restarts into Pocket Daily. Before executing
the real `enterDeepSleep()` path it verifies a saved-network return marker.
The result contains the run number, same-attempt pre/post frame-release heap,
and baseline/final BLE counters. `GET` of the same route retrieves the checked
SD record after reconnection. Interrupted experiments are marked failed and
return to the pre-armed LAN route instead of repeating the experiment.

The default trial software-restarts after display/storage preparation and
before CPU sleep. `--timer-sleep` explicitly selects X3-only real deep sleep
with a three-second timer; acceptance additionally requires `timerArmed`,
the live timer wake cause, and `lastResetReason=deep-sleep wake`. X4 timer
requests are rejected because its battery latch can remove CPU power. These
endpoints, boot execution and timer helpers compile out without
`ENABLE_DEV_REMOTE_FLASH`. The SD record is fixed-size developer evidence
(at most 192 B resident); no new production heap buffer is reserved.

The runner preserves failed evidence, applies the >50 KiB sampled runtime
heap target and requires a completed READ_LIST when `--require-exchange` is
set. A completed list is not an OFFER test. Between runs it respects the app's
60-second reconnect cooldown. Logs/identifiers are confined to ignored build/.
Formatting is performed and reviewed before this validation/build pipeline.

First installed structural candidate: `8925b142-wf7a0c1de`, SHA-256
`68cfe26711105650b252dfeafc7880643bba0c89ef6dc56f55d795a91f94e65d`.
Four software-return X3 cycles completed without physical interaction. Each
recovered free heap from 80,592 B to 133,844 B (largest block 61,428 B to
114,676 B), opened BLE and completed one Mac reading-list exchange, then
returned automatically to Same Wi-Fi. Sampled minimum free heap was at least
67,776 B. Mac UI changed from unconfirmed pairing to a confirmed Bluetooth
position exchange. No OFFER occurred, no current was measured, and these
reboot-separated trials do not establish reversible same-boot standby stability.

The next candidate, `8925b142-wa5f2ccae`, completed two real X3 deep-sleep
trials with timer wake. Both recorded `timerArmed=true`, `timerWake=true` and
`lastResetReason=deep-sleep wake`, one completed Mac READ_LIST and automatic
Same Wi-Fi return. This proves the requested timer return path, not BLE wake
from deep sleep. Its SHA-256 is
`6c5a2e8bc14667d22d0e21d346c35bd49768731acf61b3e9415306625328ef90`.

Final source verification: 882/882 host tests, 48 Python pipeline/upload/route
tests, strict cppcheck 2.11 and a warning-free default firmware build passed.
Isolated source mutations removing the terminal RenderLock or arming a request
before verified persistence failed their respective regression tests. Using
current compiler commands, preprocessing main, endpoints, the cycle module and
HAL power management without `ENABLE_DEV_REMOTE_FLASH` confirmed that the
developer cycle route, record and timer helpers are absent. This is not a
production-release build or X4 sign-off.

Final installed candidate: `0.1.0-dev-fix-ble-window-cache-preflight-8925b142-w1aa7db7b`,
6,394,416 bytes, SHA-256
`dbf60622bda2c178ada4ea9bc183ec5df5f2a1a51e9b877974057ac75c8b4f90`.
The complete `--build --cycles 3 --require-exchange --timer-sleep` pipeline
passed, including remote installation and three real sleep/timer-wake cycles.
Every cycle recovered exactly 52,272 B (80,592 -> 133,844 B free), completed
one Mac READ_LIST, sampled at least 67,956 B free / 61,428 B largest block
during BLE, and returned to the same identified reader/version on Same Wi-Fi.
The final independent status read reported STA and `deep-sleep wake`.
Local evidence is in ignored `build/ble-cycle-runs/final-pipeline.log` and
`build/ble-cycle-runs/final/`. Across the three candidates there were four
software-return and five real timer-wake successes; only the final three
exercise the final firmware artifact. Documentation was updated after that
artifact was built. No production release, optical screen inspection, OFFER,
current measurement, X4 or same-boot restoration acceptance is implied.

### Power integration and board distinction

- The default resolved SDK lacks PM, tickless idle and BLE modem sleep. Use
  a separate experimental build first; rebuilding the SDK and checking its
  resolved header is necessary. Firmware-only CPU clock reduction is insufficient.
- Replace the BLE-wide normal-speed lock and 10 ms polling with IDF PM locks
  scoped to actual work plus task notification/deadline waits. Notification
  pacing and its current retry-by-loop-count timeout must become time-based
  before the loop cadence changes (`ReadingSyncSession::MAX_SEND_RETRIES`).
- Use the installed C3 main-crystal power-save configuration first unless an
  external sleep crystal is verified. It can maintain BLE but keeps the main
  crystal powered. Connection intervals and advertising intervals need energy
  versus latency measurements; there is no measured optimal value here.
- `BoardConfig::XTEINK_X4` declares GPIO13 as a battery latch;
  `BoardConfig::XTEINK_X3` declares it as the SD power switch. The comment in
  `HalPowerManager::startDeepSleep()` calling both battery MOSFETs conflicts
  with those profiles. Do not infer X3 CPU power loss from that comment or
  change both boards' GPIO13 policy together. Preserve the X4 latch during
  connected standby. X3 SD power can be managed only through storage ownership.

The installed IDF v5.5.5 explicitly supports modem sleep + automatic light sleep
to keep Bluetooth connections. Calling `esp_light_sleep_start()` directly is
not an equivalent implementation. See the version-matched
[sleep documentation](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32c3/api-reference/system/sleep_modes.html#wi-fi-bluetooth-and-sleep-modes).

The app already keeps a pending connection to the remembered reader
(`pocket-daily/Sources/Sync/ReaderBluetoothLink.swift:84`); the existing bonded
READ1 exchange can be reused without a hotspot or Mac Wi-Fi change. iOS
background execution still needs separate device acceptance. Always-on reader
advertising alone does not prove an arbitrary suspended app will run on demand.

### Verification before calling the solution complete

Record pre-quiesce, post-release, radio-ready, exchange-minimum, radio-closed
and post-restore heap **and largest block**, with boot/attempt identity. RTC
counters alone do not identify a fresh attempt after power loss. For an X4
terminal power-off test, a bounded SD result must be flushed before shutdown;
do not log on each idle pass. No credentials or device IDs belong in commits.

Test render completion versus queued updates, callback shutdown, storage/file
ownership, allocation failure during restore, and button interruption. Use
fault injection against the production transition logic. Then exercise at
least 100 Reading <-> standby cycles on X3, checking completed READ_LIST/OFFER,
fonts, frame correctness, watchdog and absence of cumulative heap/block loss.
Keep the project's >50 KiB runtime heap target visible: merely passing the
existing lower BLE survival floors is not general hardware acceptance.

First isolate functional correctness using the one-way sleep path. Advance to
reversible standby and power experiments only after that has hardware evidence.
X3-only battery-level comparisons can reject gross drain; precise current,
small energy savings and X4 behavior remain unmeasured without the corresponding
equipment/device. None of those gates is satisfied by the compiler accounting.

## Hardware acceptance using only the X3

1. Record exact staged artifact hash/version and have the owner install it.
   Preserve the baseline artifact for rollback. No installation is implied by a build.
2. Keep Mac Wi-Fi unchanged. Reuse its existing bonded reader link. The app's
   Direct connection action requests a hotspot handoff and is not this test.
3. Exit Sync, reach Pocket Daily, close a book back to Pocket Daily, and perform
   a sleep/wake cycle. Test each trigger independently. Record whether an
   exchange completed in the app, then return the reader to Same Wi-Fi and
   read `/api/status.readSync`. Compare counter deltas rather than assuming
   the retained counters began at zero. A memory refusal is a valid safe result.
4. Repeat at least ten cycles; record start/open/min/closed free heap and largest
   block, connection/list/offer deltas, reset reason, button response, and font
   rendering after returning to a book. A successful connection alone does not
   prove a completed list/offer. Test stock Home separately: it does not host BLE.
5. Only after functional stability, compare matched 24–48-hour baseline and
   experimental standby intervals, without USB charging, at similar starting
   battery level, temperature, content and connection activity. Read endpoints
   only at interval boundaries; Wi-Fi polling would contaminate the comparison.
   Repeat in reversed order. Battery percentage/voltage are coarse proxies,
   not mA measurements or reliable small-savings evidence.

A future experiment should separately measure disconnected advertising, bonded
idle connection, completed exchanges, phone absent, app suspended, and button
wake. Accurate current and energy require an external battery-side instrument;
USB input readings include charging/board effects. X4 needs separate acceptance.

## Local verification of the cache change

- Formatting and strict cppcheck 2.11 passed; default firmware build completed
  with zero compiler warnings/errors.
- 871/871 host tests and 19 sync-route checks passed. Six new tests exercise
  the production memory-preflight function with controlled heap/cache/lock
  substitutes. Omitting cache release, omitting the render lock, or bypassing
  the post-release floor each makes the regression suite fail.
- Host substitutes establish ordering and admission behavior, not actual cache
  recovery, FreeRTOS concurrency, battery savings, or physical radio behavior.
- Candidate version: `0.1.0-dev-fix-ble-window-cache-preflight-8925b142-w84ab574e`.
  Size: 6,389,248 bytes. SHA-256:
  `2eddedb68cd4d12733e234deca44b4788ca6b3c20cf37097677979aac05c8c23`.
  This is a developer test artifact, not a production release.

The candidate was staged to the X3 over Same Wi-Fi: receiver acknowledged
6,389,248 bytes and CRC32 `CBA96CEE`, then publication to `/update.bin`
succeeded. The owner's subsequent installation was confirmed over Same Wi-Fi
by the exact candidate version string. Its retained wake sample reports
85,824 B free / 61,428 B largest block, still LOW_MEMORY, with zero opened
windows. This is 5,724 B above the earlier sample, but the activity/history
was not controlled, so it is not a measured cache-reclamation delta. A fresh
controlled sleep/wake trial and completed BLE exchange remain pending.

After the owner reported the requested sleep/wake cycle complete, the same
candidate was reached on Same Wi-Fi with uptime 18 s and software-restart reset
reason (the Sync route restarts the reader). All readSync fields were identical:
opened 0, skipped 1, wake/low-memory, free 85,824 B, block 61,428 B. Counter
deltas were zero. This confirms no recorded successful exchange; it does not
establish a fresh attempt or retention across the entire sleep/wake sequence.
The latest retained sample is 12,480 B below the admission floor. Do not repeat
this manual cycle as proof without diagnostics that distinguish attempt age and
RTC reset/retention, and record pre/post cache-release samples in the same attempt.
