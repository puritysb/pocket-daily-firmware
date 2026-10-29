# Explicit Wi-Fi production trial and return

Implemented 2026-10-27; extended 2026-10-28 with a finite normal-reader plan.
R5 established the same production boot/HTTP/transfer return on X3 and X4.
R6 `build/release-automation-20261028/{x3,x4}-reader-observe-002` additionally
passed20 completed English pages, native close/reopen position continuity,
run-owned missing-file recovery, normal sleep teardown and actual deep-sleep
RTC-timer return on both devices with production SHA
`52edb35af09d8d0814bc5078f7b0973fef2a6efc5fa3dea696df048d0a59237a`.
This is not a production post-wake reader-resume, physical SD extraction,
optical or mechanical-button claim. The current app handoff owns restoration
status; a successful trial alone never means the device is restored.

## Activation boundary

Normal `gh_release` contains an inactive boot consumer and no trial/remote-flash
HTTP endpoint. Only an already installed developer image can provision the
transaction through `GET/POST/DELETE /api/pocket/v1/dev/trial-lease`.
Every new developer request requires the enrolled `deviceID` query, including
image pieces and installation. This is a wrong-target guard, not cryptographic
LAN authentication. The shipping app never invokes it. Normal staging still does not install an
image, and the on-reader second confirmation remains unchanged.

The basic POST uses exactly `{run, seconds, imageSHA256, returnSHA256, backupAcknowledged,
installAcknowledged}`. The optional reader plan adds exactly the three fields described below. The newer explicit auto-wake plan additionally permits only `readerAutoWake:true` (ten fields total). `run` is a fresh nonzero UInt32, `seconds` is 30..180,
SHA is 64 lowercase hex digits, and both acknowledgements must be true.
It requires idle Same Wi-Fi, a validated `/update.bin`, one of two observed OTA layouts: X4 0x10000/0x650000 with
6,553,600-byte slots, or X3 0x10000/0x780000 with 7,798,784-byte slots, and no previous NVS lease record.
The staged file must have the new `POCKET_TRIAL_LEASE_V3` capability and match
its complete hash. The complete return-slot hash must also match the host backup. Old a085 is refused; a marker alone is not a source attestation.
The host must bind the candidate and return image to reviewed build receipts.

Before installation the producer hashes the complete running return slot and
persists/reads back a 116-byte CRC-checked record in its own `pdtrial` NVS
namespace. It writes the existing one-shot Sync marker. Installation remains
one explicitly authorized developer POST, never a transport-completion action.
The accepted response precedes flash. An unknown POST is never replayed.

## Return behavior

The first matching candidate boot verifies both images, commits Running and
arms one SDK-owned timer before GPIO, SD, display or Wi-Fi initialization.
The callback writes a run-bound RTC ticket and restarts; it performs no NVS/SD/flash I/O. A subsequent candidate boot in Running or
Returning verifies and selects the preserved slot before SD initialization.
Slot-selection attempts are recorded and limited to two; selector failure
stays up without a reset loop. A record-commit failure still makes one attempt
to return to the already verified image rather than begin an unlimited trial.
No bootloader is changed. A selected image must still pass the stock bootloader.

During a lease the normal flasher and native OTA installer cannot overwrite
the return slot. If the normal sleep path runs, the HAL adds a five-second RTC
wake on X3 or USB-powered X4. Other power conditions restart into recovery.
That wake goes directly to the return image; it is not production reader
quick-resume or a physical-button observation.

The return boot verifies the whole original slot, reports Returned and uses
Same Wi-Fi once. A trial never entered is Failed, not a successful return.
A failure code survives the return. GET (schema3) reports actual running/next app partition metadata and the run, phase, bounded duration,
slot addresses, both image hashes and reset/wake evidence without credentials.
GET with `imageOffset` exports at most4KiB from the current running app slot,
never arbitrary flash/NVS/bootloader addresses. This lets the host independently
back up and compare the complete return-slot bytes. DELETE accepts only
`{run, backupAcknowledged:true}` for a matching terminal record; pending records
and other runs are preserved. Existing malformed records also block provisioning.
A pending run permits only a Sync restart while actually running its preserved
return slot, so an ambiguous preparation can settle without replaying a flash.
File Transfer and a different running slot remain blocked until the run is terminal.

## Finite normal-reader plan (V1)

`plan --reader-epub /private/fixture.epub --seconds 180` freezes a1-byte..4MiB
run-owned EPUB, its complete SHA256 and20 requested completed pages. It requires
`POCKET_TRIAL_READER_V1` in the reviewed production image. Stage publishes only
`/Pocket trial <run>.epub` plus the pinned firmware, using fresh UUID staging.
Start adds `{readerPages:20, readerBytes, readerSHA256}` to the six-field POST.
The developer producer hashes the fixture and commits a separate CRC-checked
`pdtrial/reader` record. No production HTTP reader-control route is added.

The image-verified production consumer hashes that exact path before opening.
It uses the ordinary EPUB activity, completed-page witness and normal pageTurn
method. Native history and reading-progress writes remain enabled; this is not
the history-isolated developer walk. It then leaves/reopens the book, compares
spine/page/visible offset, closes it, temporarily renames only its own file,
observes the normal missing-file open failure, restores byte-identical content
and reopens at the saved position. Only all successful steps can reach normal
sleep. The existing lease adds its five-second timer return. Failures return
with a failed reader stage, or the bounded lease returns with incomplete evidence.

Developer GET includes optional `reader` schema1/run/stage/requested/rendered/
spine/page/reopened/missingObserved/recovered. Sleeping stage7 plus20 rendered,
all three flags1 and independent resetReason8/wakeCause4 is the specific V1
acceptance predicate. Repeated paints and backward positions never count.
A sampled HTTP production response is not required while the radio is off in
reader mode; the checked boot/image lease and persisted on-device evidence are
the witness. This does not establish native quick resume after a timer wake.

Before running, preserve known raw state/recent/settings/index/dirty/global
sequence, sleep-frame state, original books and known original native caches.
The normal reader deliberately changes its own progress and global history.
Afterward verify the unchanged fixture bytes, delete only its owned book/cache,
save and clear the terminal records, restore the backed-up fixed original files
and compare the preserved original cache/book subset. Cache rebuilds, an absent
file, an injected storage read error and physical card removal are separate tests;
V1 performs only the absent-file test. Never erase original caches to obtain equality.

## RTC/GPIO probe isolation

An ELF gap is not proof of free RTC memory. The pinned C3 SDK enables
`CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP` and registers the region after
`_rtc_force_slow_end` as a runtime heap. R6's first timer-only prototype missed
that condition, stalled HTTP and was quarantined. Its install preflight failed
before sending a POST; one USB/JTAG recovery restored the owned RTC span/entry,
reset the allocator and confirmed the exact prior developer image/Sync with no
pending lease. It is a failed experiment, not a wake pass.

Only the explicit non-shipping `rtc_probe_bridge` / `rtc_probe_production`
environments may reserve a diagnostic arena. The separate linker reservation
must advance IDF's real heap exclusion boundary while preserving every native
RTC object address and SDK retained-timer region. Before any provisioning,
verify both actual ELFs with `rtc_probe_policy.validate_layout` and require the
live bridge's `rtcProbe` report to prove the same boundary and intact heaps.
Require heap integrity again after provisioning. Shipping `gh_release` must
never inherit the arena flag or custom linker script. Timer-only, electrical
GPIO, native reader resume, actual contact and battery-latch cold-on are distinct gates.

## Evidence and limits

A host run must save original public/known SD/book state, current staged-image
ownership and original firmware bytes before installing a bridge. Recheck the
same enrolled identity/version on every phase. Save the actual Returned receipt,
exact original image, post-return public/known-state and cleanup evidence.
A developer bridge's inactive slot/NVS writes are not a full pre-bridge flash
restoration claim. Never transplant X4 backups to X3.

NVS/flash failure, an unavailable or changed saved network, power loss, and a
failure before the application hook executes remain separate recovery limits.
The application timer is not an independent hardware watchdog. This is a
bounded developer maintenance mechanism, not a universal fail-safe guarantee.
No existing original cache may be deleted/reconstructed to force equality.
An externally controlled electrical GPIO wake, a software wake classification
injection and a real button contact must have distinct acceptance labels.

V3 retains V2's binding of an expected deadline reset to the run using the existing product-owned
RTC no-init words with a separate magic. A restart without that ticket (except
an actual RTC timer deep-sleep wake) returns safely but records UnexpectedReset;
it cannot pass a healthy production trial. A Running record found directly on
the return slot is also a failed/uncertain run. The callback performs no NVS or
SD work. A valid one-shot maintenance return bypasses ordinary cold charge-sleep
once; subsequent ordinary boots retain the normal policy. RTC layout is unchanged.

The first X3 image-backup request was safely refused by V2's X4-only size guard.
A read-only developer layout probe established the different X3 table; no table
was changed. V3 validates both app partitions against an exact allowlist, includes
the larger complete return slot in its SHA and rejects mixed/unknown layouts.
Host plans bind model-specific slot addresses/size and check actual metadata before
export. Existing V2 X4 hardware results and V3 production results remain distinct.

## Explicit host runner

Use frozen images and an enrolled private configuration. All mutation commands
require `--execute` and fresh output directories. Do not use the mutable staged
`firmware/update.bin` to identify an image. Example (paths are placeholders):

```sh
python3 scripts/dev_trial_lease.py plan --config /private/reader.json \
  --model X3 --firmware /private/production/firmware.bin \
  --expected-version '<exact-installed-developer-version>' --seconds 120 \
  --output /private/trial
python3 scripts/dev_trial_lease.py image-backup --plan /private/trial/plan.private.json \
  --return-image /private/bridge/firmware.bin --execute
python3 scripts/dev_trial_lease.py stage --plan /private/trial/plan.private.json \
  --backup-receipt /private/known-state/receipt.json --execute
python3 scripts/dev_trial_lease.py start --plan /private/trial/plan.private.json \
  --return-backup /private/trial/image-backup/receipt.json --execute
python3 scripts/dev_trial_lease.py observe --plan /private/trial/plan.private.json \
  --transfer --execute
```

The operator must audit the original known-state backup and both frozen build
receipts. Plan/script/client/sender/image pins and plan-wide one-use stage/start
markers prevent replay through another output directory. An unknown POST or
transfer/commit is preserved, never automatically repeated. Hashing, flashing,
booting and whole-slot return verification add time outside the configured
active timer; do not treat a temporary HTTP outage as permission to repeat start.
A host observation timeout requires saved evidence and fresh GET-only review.

A successful return is not final cleanup. Save the matching terminal record,
verify/delete only the run-owned TXT (normal file API requires exact size and
cursor0), rebuild the derived index through normal Library, and compare original
raw/public/book state. R5 generic deletion invalidated the index even for non-books and recreated
`library.dirty`. R6 limits that invalidation to supported book extensions; verify
the installed bridge contains the fix before restoring an absent dirty marker.
Never loop a deletion against an older bridge.
A single owned Library trial increments CLIX `nextFirstSeen` even after removal;
record that bounded counter difference rather than silently claiming byte equality.
Then `clear --plan ... --execute` can remove only the matching terminal NVS key;
restore the original developer artifact separately and verify original identity,
version and known state. Clear does not restore the original inactive slot/NVS
page layout or prove full SD equality.

`mode --mode sync|file-transfer` is a developer-only one-shot saved-network
restart, not an independent return watchdog. R6 fixed the observed legacy folder
response failure with bounded coalesced close-delimited JSON writes, reused SD
entry handles and explicit failure handling. It preserves the array fields and
CORS contract. A partial response is deliberately incomplete JSON, never a valid
partial inventory. Actual X4 repeated queries and early-disconnect recovery
passed; do not infer arbitrary damaged-card/SD-driver timeout coverage from it.

## Nonshipping X4 GPIO wake experiment

`rtc_probe_bridge` and `rtc_probe_production` are opt-in diagnostic environments.
They reserve a 1 KiB NOLOAD RTC arena with `scripts/rtc_probe_arena.ld` and the
ordered linker hook. `_rtc_force_slow_end` must include the arena: the pinned
C3 SDK uses the remaining RTC fast memory as heap. An empty ELF gap alone is
never permission to write memory. The pure layout/runtime guards in
`scripts/rtc_probe_policy.py` retain all native RTC object addresses and require
a healthy heap before and after any memory-only provisioning.

The diagnostic consumer accepts one run-bound GPIO continuation only after
independent raw RTC GPIO cause/mask and SDK deep-sleep/GPIO witnesses agree.
It preserves native power-button boot policy, releases the owned GPIO3 actuator
through `HalGPIO`, and observes the first normally rendered EPUB page and its
saved position. The captured bitmap is a run-owned `/.pocket/rtc-wake-<run>.bmp`;
it must be preserved as evidence and removed before restoration. Timer or
watchdog fallback never counts as GPIO success. The shipping `default` and
`gh_release` images exclude both the arena and the GPIO diagnostic consumer.

R6 trial005 refused to drive GPIO because the normal SDK sleep path retains
GPIO3 as an input (`PAD_HOLD` bit3). It returned by timer with the actuator
released; its GPIO gate remains failed. Trial006 then explicitly recognized
that observed native input hold and released only bit3 before the bounded
open-drain test. It passed actual SDK deep-sleep/GPIO wake, unchanged native
power boot policy, actuator release, normal reader position and saved bitmap.
The final independent slot return was software reset, not relabeled timer wake.
The offline [payload builder](../scripts/rtc_gpio_probe/README.md) reproduces
the exact tested820-byte binary and refuses an unreserved shipping ELF. Private source/disassembly and one-use execution journals are
retained under `build/release-automation-20261028`; do not replay an injection
or installation with an unknown outcome. Electrical wake on a USB-powered
board cannot certify mechanical switch contacts or battery-latch cold-on.

## Natural inactivity and native production timer resume (2026-10-28)

`--reader-auto-wake` requires the finite reader plan, a 360-second lease and a
candidate containing `POCKET_TRIAL_AUTOWAKE_V1`. Only the developer producer
accepts `readerAutoWake:true`; the shipping app and normal update confirmation
are unchanged. A separate checksum-checked, run-bound `pdtrial/readerwake` NVS record
is used; the V1 reader record and V3 lease binary layouts are unchanged.

After the normal 20-page, close/reopen and owned-file recovery checks, the
runner stops navigation. During the finite automated preparation it reports background work to the existing inactivity guard, so a cold first index cannot interrupt the test before its first page. After preparation the ordinary clock runs for the full idle interval. Its one-minute timeout and Quick Resume on Timeout
settings exist only in RAM. It does not call `enterDeepSleep()` for this plan:
`main.cpp` waits for the ordinary inactivity clock and `preventAutoSleep()`
conditions, records the actual timeout/elapsed values at that existing branch,
and invokes the unchanged normal `enterDeepSleep(true)` teardown. A five-second
maintenance RTC timer provides the wake source on X3 or USB-powered X4.

Only a matching completed reader plan with a recorded >=60-second idle,
actual SDK deep-sleep reset and timer wake can consume the single resume.
Consumption is committed before later boot work. Both image hashes are checked
again and a new bounded deadline is armed. The trial does not request Sync or
call `goToReader()` on this wake: the normal persisted sleep-frame/reader boot
routing executes. The resulting native reader must render the same spine,
page and visible text offset, then render an advanced page through its normal
page-turn path before returning to the preserved developer slot.

The optional terminal `reader.autoWake` object has schema1, stage6 for Complete,
`timeoutMs`, `idleMs`, actual `resetReason`/`wakeCause`, `positionMatched` and
`pageAdvanced`. Missing, failed, mismatched or repeated wake evidence never
passes. A software deadline, crash or second sleep cannot repeat production
resume. The developer producer requires exactly360 seconds for this opt-in plan; basic trials remain30..180. The unchanged116-byte lease decoder now accepts up to360 seconds. Older consumers cannot run this extension; the host requires the new auto-wake capability. At most two bounded production boots occur; this is a finite extension
of the original single-boot trial, not an unbounded lease renewal. Clearing the
matching terminal reader also removes its owned wake record.

This validates natural inactivity and production reader continuation with an
explicit timer wake. It does not certify mechanical power-button contacts,
battery cold-on, optical glyph coverage or unpowered SD recovery. All changed
settings, reading state, owned files and active firmware must be restored and
verified separately; inactive OTA bytes are not full-flash restoration.
