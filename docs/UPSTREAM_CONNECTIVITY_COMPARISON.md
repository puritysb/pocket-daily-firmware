# Upstream connectivity comparison — 2026-09-21

## Source audit after installed bootstrap — 2026-09-22

### External reports checked against this tree

- [CrossPoint #1160](https://github.com/crosspoint-reader/crosspoint-reader/issues/1160)
  reports very slow/failed small uploads from macOS. Its August 16 closure
  comment says stale, not fixed. A March comment reports mixed results while
  externally powered/debugging, which changes multiple conditions and is not
  a reproducible fix. Do not equate closed status with resolved transport.
- [CrossPoint #3593](https://github.com/crosspoint-reader/crosspoint-reader/issues/3593)
  concerns an X4 opening a large local book hierarchy and links to #3600.
  It is not evidence for this X3's pre-transfer TCP failures.
- [ESP-IDF #9059](https://github.com/espressif/esp-idf/issues/9059) concerns
  C3 modem-sleep instability on a 2022 v5.0-dev build with
  CONFIG_ESP_PHY_MAC_BB_PD enabled. Our effective C3 dio_qspi sdkconfig does
  not define that option, and `CrossPointWebServer.cpp:149` already calls
  WiFi.setSleep(false). The installed Arduino implementation maps that call
  to WIFI_PS_NONE (WiFiGeneric.cpp:770–795); STA startup reapplies its stored
  setting (STA.cpp:122). The call's result is currently unchecked, so actual
  runtime power-save state is not proven. This report does not justify blindly
  repeating the sleep-disable patch or downgrading the SDK.

Current platform is pinned to 55.03.311. The official
[Arduino release notes](https://github.com/espressif/arduino-esp32/releases/tag/3.3.11)
identify Arduino 3.3.11 / IDF 5.5.5; old IDF or other-chip failures are not
automatically applicable. No evidence gathered here establishes a matching
SDK defect. No firmware, power setting, router setting or radio mode was
changed during this review.

This audit does not flash, rebuild, change the reader's connection mode or claim
the physical cause is established. Installed `w998f2f9d` is the fixed baseline;
the control-plane observations are in `TRANSFER_BENCHMARK.md`.

### Unrequested reconnect is a fork-only confounder

The pinned upstream activity (`aa994cf7`, lines 306–347) observes association
state, relies on driver auto-reconnect for actual disconnection and abandons
only after sustained association loss. It does not probe a gateway admin port.
Current `StaRadioWatch.cpp:98–106` instead calls a blocking gateway TCP probe
and may return Reconnect even while WiFi.status() says connected. The current
`RadioHealthPolicy.h:25–27,40–58` specifies a 10-second period, a 1-second
select timeout and a reconnect on every third timeout. The activity applies
that result with WiFi.reconnect() (`CrossPointWebServerActivity.cpp:651–652`).
Uploads and recently handled client requests suppress it, but failed incoming
connections do not refresh lastClientActivityAt, so failed discovery is exposed.

Successful gateway contact proves that one path worked; timeout does not prove
the radio is broken. A filtered/slow admin port and a deaf reader produce the
same input to this policy. Existing tests assert escalation after three false
results; they do not distinguish these causes. On this Mac, three read-only
TCP connects to the current gateway's port 80 completed in 5, 3 and 4 ms. That
rules out a consistently unavailable admin service from the Mac during this
sample, not reader-side timeouts or this branch firing. The installed build's
NetHealth note functions compile to no-ops without ENABLE_DEV_NETWORK_DIAGNOSTICS,
so absence of a new log cannot establish that no reconnect happened.

Disposition: treat probe-triggered reconnect as a suspect intervention, not a
proven repair. Do not add more reconnect levels or interpret timeouts as an
installation request. A future isolated correction should restore passive
association tracking rather than letting an application-port probe tear down
a connected radio. This audit alone does not justify another device flash.

Local correction following the audit: `StaRadioWatch` no longer opens gateway
sockets or exposes a Reconnect action. `AssociationPolicy` preserves the
upstream-style five-minute actual-disconnection grace and driver recovery,
including wrap-safe timing and a disconnection first observed at uptime zero.
This eliminates the identified intervention by construction. It is not installed
and cannot explain retrospectively which branch ran on the physical reader.

### Optional listener complicates memory comparisons

`LiveStudioService.cpp:27–39` allocates a listener when the pre-allocation free
heap crosses `kMinListenerFreeHeap` (current source: 16 KiB, not the older
12 KiB documentation claim). No client subscription is required to allocate it.
Thus freeing unrelated memory can cross the threshold and enable a new owner;
one must compare service sets as well as heap snapshots. The installed baseline
advertised push/port81; previous observations of `w67fff974` advertised poll.
No current evidence attributes all failures to this listener. With no attached
client, status pushes are guarded (`LiveStudioService.cpp:120`) and frame events
are subscription-gated; an always-running broadcast is not established.

### Investigation boundary

No more upload/SD/mode-change loops while these confounders are unresolved.
Keep radio-buffer policy, reader position and home network fixed. Separate
association/reconnect behavior, optional service residency and application
payload handling. The installed transfer counters cannot diagnose pre-accept
failures: attempt=0 is evidence that the data-plane experiment never began,
not evidence that SD or stream processing passed.

## Conclusion and confidence

Do not rewrite the network stack yet. First isolate Pocket Daily's permanent
memory costs, correct the pre-radio resource lifecycle, and compare transports
on the same build. Upstream has genuine reported defects and fixes, but those
reports do not establish the cause of our port-82 failure. One successful
experimental upload is recovery evidence, not proof of a unique root cause.

This investigation changed no executable code, flashed no reader, and did not
change the Mac network. A single status request to the previously supplied LAN
address failed to connect. That observation cannot distinguish a reader that
left File Transfer from a network fault. New physical comparisons remain open.

## Reproducible baselines

- Pocket HEAD: `b8e38e39be1b6fd043af57f57b14e193d1fa836f`, plus the existing
  uncommitted repair set. Installed image and physical results are recorded in
  [CORE_CONNECTIVITY_REPAIR.md](CORE_CONNECTIVITY_REPAIR.md).
- Locally available upstream master: `aa994cf7bf8fb3fd0e08c7c264ae1802c818ab86`
  (2026-09-05). Latest GitHub stable release checked during this investigation:
  [1.6.0](https://github.com/crosspoint-reader/crosspoint-reader/releases/tag/1.6.0),
  published September 5; X3/X4 asset `crosspoint-1.6.0-x3-x4.bin`, 5,437,760 bytes.
  Pocket's `1.6.6` label does not mean upstream 1.6.6 plus a small patch.
- Git merge base with that upstream snapshot: `2754a5ff01644d36cf0a17db98f28408666ba518`
  (2026-06-29). Some later behavior is hand-ported; ancestry alone does not
  establish whether a fix is present. Check code semantics.
- Upstream develop observed via API: `8c84ef3268fd56147ae1625e04f773f0f50b7720`.
  It is not the stable-release baseline and was not installed or built here.

## What upstream actually fixed

1. [#3035](https://github.com/crosspoint-reader/crosspoint-reader/pull/3035),
   merged August 15: release rebuildable SD-font caches **before** network
   allocations. The author isolated a Korean-font-dependent X3 startup abort
   by switching fonts. Pocket has a different font system: it unloads the
   selected family in `CrossPointWebServerActivity.cpp:518`, but the STA path
   calls `WiFi.mode(WIFI_STA)` at line 240 first. This is not equivalent
   pre-radio protection. Moving cleanup earlier must retain render locking
   and prevent the network chooser from immediately repopulating large caches.
2. [#2990](https://github.com/crosspoint-reader/crosspoint-reader/pull/2990),
   merged August 12: do not register the File Transfer serving task with the
   task watchdog; synchronous HTTP/ACK waits can exhaust its budget. Pocket's
   current STA path likewise does not register it; private AP separately arms
   a watchdog. Reapplying this fix is not an explanation for current STA stalls.
3. Upstream's [tuned C3 build](https://github.com/crosspoint-reader/crosspoint-reader/blob/aa994cf7bf8fb3fd0e08c7c264ae1802c818ab86/platformio.ini#L117)
   explicitly rebuilds SDK libraries with smaller timer stacks and Wi-Fi IRAM
   options disabled. Pocket lacks this configuration and cache integration.
   **Important correction:** installed 55.03.311 SDK config already has both
   `CONFIG_ESP_WIFI_IRAM_OPT` and `CONFIG_ESP_WIFI_RX_IRAM_OPT` disabled.
   Therefore upstream's historical 32–37 KB saving is NOT an available gain
   we can promise by copying these flags. Build configuration must be compared
   against effective SDK headers and ELF, not only project.ini text.
4. [#3397](https://github.com/crosspoint-reader/crosspoint-reader/pull/3397)
   upgrades Arduino 3.3.7/IDF 5.5.2 to 3.3.11/5.5.5 and carries cache/linker
   integration for rebuilt SDKs. Pocket changed the platform URL but does not
   contain that integration. Do not blindly copy an older framework-patching
   hook into the shared PlatformIO installation. Test in an isolated package
   environment and verify effective settings/link addresses.

## Measured or source-confirmed differences

The local `sta_recovery/firmware.elf` was inspected using ESP toolchain `size -A`
and `nm -S --size-sort -C`. These are link-time footprints, not measured peak
heap or guaranteed reclaimable amounts. Flash-resident `0x3c...` symbols were
excluded from DRAM accounting.

| Item | Current Pocket evidence | Interpretation |
| --- | --- | --- |
| ESP timer stack | SDK header: 8192 B; upstream tuned: 4096 B | 4096 B reservation difference; validate callback high-water marks before reducing |
| FreeRTOS timer stack | SDK header: 4096 B; upstream tuned: 2560 B | 1536 B reservation difference; combined timer difference is **5632 B**, not the upstream comment's approximate 7 KB |
| Main loop stack | `src/main.cpp:51`: 16384 B; SDK default 8192 B | Additional 8192 B reservation, justified by prior dashboard stack overflow; cannot safely halve without flattening/measuring call chains |
| Shared dashboard state | `src/agentdeck/protocol.cpp:20`, ELF `AgentDeck::g_state`: 14220 B | Permanent RAM even in File Transfer; also backs offline Pocket cards, so do not simply delete/disable AgentDeck |
| Provider OTA decode | `src/agentdeck/ota_ws_receiver.cpp:68`, ELF: 4096 B | Separate always-resident buffer from the common 4096 B firmware staging buffer; candidate for exclusive ownership/reuse after concurrency audit |
| Provider outgoing queue | `src/agentdeck/ws_client.cpp:34`, ELF: 1200 B | Permanent regardless of whether this provider is connected |
| Bluetooth code | ELF IRAM contains linked BLE controller functions (`r_lld_*`, etc.) | BLE deinit can return runtime allocations, but does not unload linked instruction RAM; a LAN-only comparison build is needed to quantify the complete delta |
| Total linked IRAM | 92100 B `.iram0.text`, plus 60 B end/alignment | Shares physical C3 SRAM with data; ordinary static-DRAM build summaries alone miss this pressure |
| HTTP upload buffering | Pocket 1024 B vs upstream 4096 B | Local HTTP path is modified too; it is not an untouched upstream control |

The three listed provider/shared-state objects total 19,516 B. That is a
footprint inventory, NOT a promise to reclaim all of it: the offline deck
needs state while active, and safe transfer-time release needs ownership and
reload rules. Do not add these numbers to old heap snapshots as though all
were independent, recoverable runtime allocations.

## Three different update paths

- Upstream browser transfer: HTTP multipart / WebSocket uploads.
- Pocket companion transfer: port 82 `POCKET-PUT/1`, resume, SD ACK and verified
  commit. The latest physical success tested this path, not upstream transfer.
- Reader's internet OTA: upstream snapshot uses HttpDownloader/wolfSSL to stream
  into the update partition; Pocket still calls `esp_https_ota`/mbedTLS in
  `src/network/OtaUpdater.cpp`. HTTPS reports must not be conflated with LAN
  upload failures. For the user's workflow, let the Mac fetch/validate the
  artifact and send it locally; do not require a reader TLS migration just to
  make companion updates work.

Partition CSVs in the compared source snapshots match (two 0x640000 app slots).
That is a prerequisite, not proof of this reader's actual partition table or
safe rollback. Never replace bootloader/partitions during the comparison.

## Discriminating experiments, in order

Use the same X3, SD, reader position, router, files and warm/cold entry sequence.
Do not change RSSI conditions to make one variant look better. Record image
hash, SDK effective settings, free/minimum/largest/DMA heap at each phase,
loop service gaps, SD write time, ACK round-trip time, resets and final hash.
Diagnostics must use small fixed counters, not an always-on multi-KB log ring.

| Step | Change one variable | What it distinguishes |
| --- | --- | --- |
| 1 | Current image: ordinary HTTP upload vs port-82 stream, one client at a time | Protocol/service-path difference, not pure upstream vs fork |
| 2 | Same Pocket source/SDK: default vs bounded Wi-Fi buffer policy, identical sender pacing | Whether the experimental driver cap is necessary; current evidence combines changes |
| 3 | Same source/transport: provider state released for transfer, then separately a LAN-only build | App/provider residency vs BLE linkage; retain features in product until effects are measured |
| 4 | Same source: verified timer-stack configuration; separately pre-radio cache cleanup | Incremental heap and entry-lifecycle effects without a broad rewrite |
| 5 | Official pinned X3/X4 release using its native transport | Whole-product control; requires confirmed SD recovery and user-coordinated install, because official firmware has no Pocket dev-flash endpoint |

Transfer files: 1 KiB, 256 KiB, then the same ~6 MB artifact stored as inert test
data; verify downloaded hash as well as completion response. Never flash a
renamed diagnostic payload. Repeat successful cases at least three times,
exercise one intentional interruption where supported, and include idle dwell.
Only then test a distinct real firmware's install, same-device version check,
and automatic saved-network return. One successful run is not release sign-off.

Existing 512-byte/10 ms pacing is a recovery baseline, not the final performance
architecture. It imposes a sender-side speed ceiling. Measure SD and network
wait time before selecting adaptive pacing or a larger credit window. Do not
enlarge buffers on a 9 KB free-heap device just to improve a benchmark.

## Recommended architecture

Keep one bounded LAN transport with resumable staging and explicit validation.
Make reader/provider caches and optional services obey a pre-radio memory
contract; separate offline deck state from provider connection/OTA state.
Allocate or borrow provider scratch only while that provider owns the operation,
with deterministic release before transfer. Preserve tested stack safety until
the synchronous dashboard/feed chain is measured or flattened.

Use content/UI-pack application for changes supported by that schema; use real
firmware only for executable behavior. Developer installation may automate the
explicit flash/rejoin/version-check loop, but production keeps reader consent.
An always-on background radio is not assumed: it has battery and memory costs.
Native-IDF rewrite, extra persistent network tasks, and blanket upstream merging
are not justified by the current evidence.

## Implementation follow-up

The subsequent repair moves SD-family release under RenderLock ahead of all
File Transfer mode branches, before STA initialization or hotspot startup.
Calibre's independent entry does the same, and its defensive pre-server release
now also holds RenderLock. The existing post-chooser release remains as a guard.

The provider OTA receiver no longer reserves a separate 4096-byte decode array.
Chunk decode/write/hash and pulled-image hash borrow the existing flasher
scratch only for their synchronous main-loop calls. The WS client is pumped
cooperatively by PocketDailyActivity and disconnected on exit; File Transfer
is a different activity. These calls neither pump network callbacks nor retain
scratch contents across events. Validation/flash reuses the buffer only after
decode or hashing has finished. A static assertion checks the decode capacity.
No wire format, image validation, offline card state or stack size is changed.

The default build's static RAM is now 131064 B, down exactly 4096 B from the
previous 135160 B baseline. ELF inspection confirms the provider decode array
is absent and the common 4096-byte staging array remains. This is a link-time
saving, not a guaranteed runtime heap gain. Subsequent physical CLI/X3 testing
installed distinct build `w67fff974` wirelessly and verified automatic saved-Wi-Fi
return and exact version on the same reader. Post-install 64 KiB reception passed
CRC but remained slow (about 2.2 KiB/s). Heap varied from 9116 to 10976 B;
diagnostics initially hit the memory guard but later retrieval identified an
older build's saved panic, not a crash of this installation. See
`CORE_CONNECTIVITY_REPAIR.md`. App delivery, X4 and long-run stability remain open.
