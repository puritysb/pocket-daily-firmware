# Pocket Daily Firmware Release Checklist

## Current local reading repair — 2026-10-29

Production0.1.0 gh_release, 6,372,784B, SHA
`b423bd02c0e61f9fc471203ac370fd750ab07af7a7839b9cbad8e416a256e7cf` is frozen in `build/reading-memory-20261029/fresh-samples-004/artifacts/production.bin`.
The requested reading-memory repair, fresh measurements and scoped restoration
passed on both models. Full release acceptance/publication remains on hold.

| Scenario | Lowest observed free heap | Lowest largest block | New saved renders | Natural idle |
|---|---:|---:|---:|---:|
| X3 Original English | 61,280 B | 42,996 B | 24 | 60,072 ms |
| X3 Korean regression | 59,036 B | 42,996 B | 24 | 60,004 ms |
| X4 Original English | 69,424 B | 47,092 B | 24 | 60,016 ms |
| X4 Korean regression | 65,536 B | 51,188 B | 24 | 60,016 ms |

Each reading scenario has distinct native20 plus saved24,
with CRC-checked fresh tail16, cached reopen/owned absence-recovery and actual
SDK8/timer4 position/next-page proof. The free-heap gate remains >51,200B.
Original profile2/raw9/known cache/public/whole books16/7 are restored with log0;
workers and awake assertions are closed. No whole-SD/NVS/flash claim.

The root cause was parser/CSS/layout state overlapping glyph/grayscale memory;
Section drains a bounded2KiB tail first. Separate instrumentation fixes generate
new records for unmarked turns, reject duplicates and flush native-wake records.
Older renderMemorySamples20 counters could reuse data and cannot certify20
separate render peaks. Same-version cumulative minima include older BINs and are
not current-image gates. Old functional results and below-gate samples remain.

Host1149/strictCppcheck2.11/default·gh_release warning0/seam pass; freshness mutants3
and epoch negative cases6 detected. Render frames224/176B retain the same culling
storage, combined stack400B versus368B before; no new per-page heap buffer.
Existing caches reused, app shipping185 inputs and signed IPA/PKG unchanged.
Full details: [current matrix](../../pocket-daily/docs/RELEASE_VERIFICATION.md).
Broader transport/radio/physical/external gates below retain their exact-image
scope; R13 transport/Mac results do not accept this new BIN. Physical power is
excluded by user instruction, unverified. Source commit/push was separately
authorized on2026-10-29; no distribution upload, release tag or public release.

## Commit-preparation checks — 2026-10-29

Reused-cache Host1149/Python308/strictCppcheck2.11/default·gh_release warning0 pass.
CI clang-format21 changed four files: runtime headers BoundedFileDownload and
TrialWakePolicy plus two tests. The formatted-source rebuild is6,372,800B, SHA
`2e599a07359772f2067aaed438b75c84e5310334533bd75d53960056b49ede1a`.
It was not installed or hardware tested; do not inherit b423 exact-BIN acceptance.
The frozen candidate/source-pin evidence is historical, with these later formatting
changes recorded in `build/commit-push-20261029/format-only.patch`. Staging
`firmware/update.bin`/`LATEST_BUILD.txt` was restored to frozen b423. No release tag.
The original SDK gitlink remains pinned; the local storage change is reproduced
by tracked `scripts/storage_sdk.patch`, not a private SDK commit.

## Historical original-book memory failure

The old818a...715bc observed X3 36,612B/X4 44,588B below the unchanged reading
gate; it was not accepted. The46ba one-buffer repair also failed. The0f8 initial
improvement lacked independent20-render proof and was remeasured after the
freshness fix above. Old failed receipts and the annotated prior app matrix are
preserved. Do not overwrite them with current success.

## Previous R13 candidate and expanded closeout — 2026-10-29

Measurement correction: the historical memory minima below describe recorded
observations, not20 distinct render samples. Its trial could count the same record
repeatedly. Use the current fresh-sample table above for the repaired reading gate;
retain R13 functional/transport results only for that older BIN.

Production0.1.0 gh_release SHA
`818a48ad291f67420d32ffffb1dd927d441547ec4a0fbcedb89448ecda4715bc`
(6,372,688 B) is frozen in `build/release-closeout-20261028/artifacts/production.bin`.
Do not inherit sign-off from older hashes below. R13 hardware work is closed;
both original readers and scoped state are restored. See the sibling app's
[current handoff](../../pocket-daily/docs/CURRENT_HANDOFF.md) before new hardware I/O.

Both exact-BIN readers passed20 EN and20 KO completed pages, normal close/reopen,
owned-file absence/recovery, initial/cache body equality and normal one-minute
inactivity followed by actual SDK8/timer4 resume, matching position and completed
next page. Digital EN/KO titles/body/test image were inspected on both models;
this is not optical panel acceptance. EN/KO reading minima are X3 61,496/59,228 B,
X4 68,052/65,932 B. Home passes at70,092/62,400 B; thresholds are unchanged.
UI, exact production FT24requests and Sync1/20MiB pieces/negative7/settings8fields
across4 orientations, and actual Mac public-interface10/10 per model pass.
Original active wb05/wd images/IDs/STA Sync, profile slots2/generation, raw9, known
cache/crash subset, public state and whole original books16/7 are restored; log
deltas0. Trial/owned files/cache/created empty folder cleared; workers/awake closed.
See final-scope-audit.json and app-final-003 receipts. Whole-SD/NVS/flash not claimed.

Host1144, targeted Python32 and full Python308, strict Cppcheck2.11,
default/gh_release builds and eight actual firmware guard mutants pass.
The full Python sweep exposed seven unregistered narrow inherited edges in the
trial integration; explicit file-scoped registrations and a negative-scope test
now pass, with seven registration-removal mutants detected. Earlier failing log
is retained. This checker-only correction did not change frozen firmware bytes.
Actual C3 frames max256B(default)/208B(production); record globals grow44B.

New app1.0.0(1) signed IPA/PKG, shipping-source snapshot and separate Release
suites are recorded in the app's [full acceptance matrix](../../pocket-daily/docs/RELEASE_VERIFICATION.md).
Unknown V4 witness/heap fields do not grant app capabilities (iOS100/macOS99 focused tests).
Physical power-button/battery cold-on are excluded by user instruction, unverified.
Direct/BLE, physical iOS/SD, optical/current/overnight, normal updater/recovery,
post-publication checks and external Store/CI gates retain their separate scope.
No upload, publication, tag, commit or push is authorized.

## R12 exact production regression and restoration complete — 2026-10-28

`build/final-transport-production-20261028` pins production0.1.0 SHA
`eb403cf1682f924d985fb8758a31ebc368631be998cc250e4293e17d4a3e4b84` (6,371,264 B).
Both X3/X4 passed20 native completed pages, close/reopen continuity, owned-file
absence/recovery, ordinary inactivity (60,001/60,014ms), actual timer wake
(reset8/cause4), persisted position and a completed next-page advance.
Separate180-second leases passed public Sync HTTP transfer/whole readback
(X3 1,184 B / X4 1,216 B), owned-file cleanup and bounded slot return.
These are fresh exact-BIN witnesses, not inherited R9 sign-off or optical tests.

Both original active wb05/wd versions/IDs/STA Sync, raw/public/known cache and
whole original books (16/3,062,182 B;7/2,540,279 B) are restored. R12 baseline log
deltas0. Owned test files/cache and trial records are cleared; all owned workers
and awake assertions are closed. See candidate-verification.json,
restoration-closure.json and owned-process-audit.json. No full-flash/full-SD claim.

Correction in R13: production Sync first-status free heap32,388/39,504 B
does not measure the Home/reading >50KiB gate; radio stages have independent
criteria below. R12 exact production legacy File Transfer was unverified: Sync exposes a different
file-content route. R11 developer FT tests do not waive this distinction.
Physical power checks are excluded by the user, not passed. Release stays held.

The tested BIN is artifacts/production.bin in R12. firmware/update.bin and
LATEST_BUILD now identify a separate default developer w7f3e1c24 image and were
preserved. Initial audit failure on that mismatch is retained; read-only
final_audit_v2 records the local staging difference explicitly and verifies
restoration without replaying writes. Do not publish the default staged BIN.
Sealed app IPA/PKG hashes remain unchanged. No upload/tag/release/commit/push.


## R11 File Transfer transport correction — 2026-10-28

See [bounded download design and evidence](file-transfer-downloads.md). Final
local gh_release SHA `eb403cf1682f924d985fb8758a31ebc368631be998cc250e4293e17d4a3e4b84`
(6,371,264 B) builds successfully. This supersedes the local R9 staged candidate;
R9 hardware acceptance does not automatically transfer to this new BIN.

Final same-source developer image wc5c8af2c passed X3 ordinary18/18, X4 immutable
ordinary15/15 and each reader's39 stress requests (34 exact full bodies, five
intentional interruptions). X4's three stale log-length assertion refusals are
preserved. The first fresh whole log read proved the102 B append and original prefix,
but its following status lookup lost connectivity. After user-prepared Sync
reconnection, three whole5,548,627 B log reads passed with152 B append and
initial prefix exact. Both R11 original restorations are now closed. Consult
`../pocket-daily/docs/CURRENT_HANDOFF.md` before any more hardware action.

Host1135, nine source mutants, strict Cppcheck/default/gh_release, app Release
protocol99 and actual C3 frames≤192 B pass. Developer heap minima16,156/13,652 B
are not a Home/reading measurement or a full pairing/AP-stage acceptance. Exact production File Transfer hardware,
physical power, optical/current/overnight and external release gates remain open.
No upload/tag/release/commit/push.


## Physical button / battery cold-on — excluded by user, 2026-10-28

A fresh read-only inventory and enrolled-reader/public-state comparison in
`build/physical-power-preflight-20261028` preserved both original wb05/wd readers
and the R9 candidate unchanged. No admitted external mechanical actuator,
supply controller or rail meter is present. Physical tests were **not run**;
no flash, power action, SD mutation or restoration transaction occurred.

The [fixture/evidence contract](physical-power-validation.md) and offline
`scripts/check_physical_power_evidence.py` are prepared:12 tests pass and six
source guard/signoff mutants are detected. Consistency never grants hardware
sign-off. There is no actuation runner/admitted fixture driver yet. X3 SD rail
power-off is not CPU loss, and the battery-off X4 needs independent mechanical
rescue. Shipping timer-only continuation cannot certify GPIO/POWERON resume.
The user excluded these physical checks from the current release-verification scope.
They remain unverified and are not signed off; upload/tag/public release remain held.

## R9 natural inactivity / production resume completed — 2026-10-28

Both X3/X4 passed production0.1.0 SHA
`da37f7fc86d1c773bc87a7182b1c284bc3de16ceebc4a313c8519076e90487ad`
(6,369,952 B), `build/auto-wake-production-v2-20261028`.
`candidate-verification.json`, `restoration-closure.json` and `worker-closure.json`
own the final scope and completion; the app current handoff has detail.

- Each completed20 native EN pages, close/reopen position and owned missing-file recovery.
- The preparation-only background-work guard then released the ordinary inactivity
  clock: X3 60,069ms / X4 60,020ms, normal timeout sleep and actual SDK reset8/timer wake4.
- Native persisted production reader routing restored the same spine/page/visible
  offset; next-page acceptance additionally required an increased completed-render counter.
- Both returned phase4/failure0 on the same run/image witness, then restored their
  original active wb05/wd firmware, raw files/public state/known cache and whole
  original files (X3 16/3,062,182B; X4 7/2,540,279B). Both existing log deltas0.
  Terminal reader/wake/lease records and owned fixture/cache were cleared.
- X4 final GET/cache timeouts were preserved and resolved by independent read-only
  verification, with no replay of restoration writes. Legacy file-download timeout
  remains a separate transport limitation. All owned workers/awake assertions closed.
- Source-pinned default/gh_release, strict Cppcheck2.11, host1124, Python40,
  eight policy guard-removal mutants and app Release protocol119 passed. Stack≤208B.
  Local firmware/update.bin and LATEST_BUILD now equal the tested gh_release BIN.

Earlier R8 X3 failed because cold reader preparation was interrupted at60s;
R8 X4's position/next-page advance predates the explicit completed-render guard.
These failed/qualified records are preserved and not promoted. The intermediate
be53 candidate was not installed. Mechanical button/battery cold-on, physical SD
faults, optical/current/overnight and other hardware/external release gates remain.
This Wi-Fi restoration does not assert inactive-OTA/full-flash/full-NVS/full-SD
byte equality. R7's broader older-image results are historical, not a complete
release sign-off for this new SHA. No upload/tag/public release/commit/push.

## R7 exact final BIN verification — 2026-10-28

The frozen shipping0.1.0 BIN SHA
`a750e14159219db52b551b15fb8377783cbfd8293f26b236d8236b96b345bec2`
has now passed core scenarios on **both X3 and X4**. Firmware source/BIN was
unchanged during the trials. Receipts and source-pinned local runners are in
`build/final-production-20261028` (R7), including `candidate-core-verification.json`.

- Each reader passed separate EN20-page and KO20-page native reading trials,
  native close/reopen position, owned-file absence/error/restore/reopen and
  normal sleep teardown with the maintenance RTC timer returning to the bridge.
- Each passed real production atomic TXT transfer/whole readback, Korean
  filename/UTF8 payload transfer/readback/delete,8 preference fields across
  four orientation values, mixed-invalid-body atomic rejection and API-value
  restoration. This is API persistence evidence, not optical layout acceptance.
- Each passed a separate1,048,593-byte transfer, whole readback while still on
  the production image, and the bounded software-deadline slot return.
- Firmware host1120 and app Release simulator preferences/layout-send/protocol119
  tests passed. No physical iPhone/iCloud/Direct/BLE result is inferred.
- Final original-device restoration completed (`R7/restoration-closure.json`).
  X3 original wb05/raw/public/16 whole files3,062,182B; X4 exact original16MiB
  preboot readback, wd startup/one USB Sync ACK/raw/public/7 whole files
  2,540,279B passed. Existing X4 diagnostic log grew102B and was preserved
  as an explicit exception; this is not full-SD equality. Owned workers and
  awake assertion closed (`R7/worker-closure.json`). See the app handoff.

Physical SD extraction/power faults, mechanical button contacts, battery cold-on,
native production reader resume after wake, natural user inactivity timeout,
optical/full-text coverage and current/latency/overnight remain unverified on
this exact BIN. The USB-only inventory found no recognized external actuator.
R6's nonshipping GPIO result is retained separately and is not promoted to
shipping-image GPIO acceptance. External uploads and public release stay held.

## Earlier R6 verification status — 2026-10-28

Release/upload/tag remains on hold. The latest app-side handoff is
`../pocket-daily/docs/CURRENT_HANDOFF.md` relative to this repository root.
Private receipts are under `build/release-automation-20261028` (R6).

- File Transfer folder hang reproduced on X4, then corrected with bounded
  1460-byte coalesced writes. Three identical complete listings and recovery
  after an early disconnect passed (`x4-folder-fix-review-001`).
- Both X3 and X4 passed20 normal production reader pages, close/reopen position,
  owned-file missing/error/restore/reopen and ordinary sleep teardown with
  timer return on production SHA `52edb35af09d8d0814bc5078f7b0973fef2a6efc5fa3dea696df048d0a59237a`.
  These are English-copy and file-absence recovery results; they do not prove
  physical SD removal, arbitrary media errors or optical quality.
- X3 restored original wb05 firmware, known raw/public/cache state, sequence6
  and CLIX counter26;16 original supported files passed whole-byte postboot
  comparison. X4 also restored its original16MiB with an independent full
  readback before boot, original wd startup/one USB Sync ACK,7 original books
  2,540,279B and postboot raw5/public4. The existing diagnostic log grew357B;
  this explicit preservation exception is not full-SD equality.
  See R6 `restoration-closure.json` and `worker-closure.json`.
- Properly reserved RTC timer-only experiment passed, including intact heap.
  GPIO trial005 safely refused the SDK's retained input hold and returned by
  timer; it remains a failed GPIO attempt. Corrected trial006 passed actual
  SDK GPIO/deep-sleep cause, native PowerButton boot, actuator release and
  normal reader page/position with a saved bitmap on a USB-powered X4. Mechanical button contacts and battery cold-on remain open.
- Full host1120, CLI18, RTC policy7, strict cppcheck2.11 and default/gh_release
  builds pass. Final formatted-source shipping SHA
  `a750e14159219db52b551b15fb8377783cbfd8293f26b236d8236b96b345bec2`
  is a build-only artifact, distinct from the frozen production hardware image
  above. The RTC arena/GPIO diagnostic is absent from both shipping builds.

## Earlier scope and status — 2026-10-27

The current product version is `0.1.0`. No candidate is signed off here and
`pocket-v0.1.0` has not been published. GitHub latest remains historical
`v1.6.6`, which the current companion and lineage 2 updater exclude. Earlier
Mac/X3 and Mac/X4 development runs are useful evidence only for their recorded
images; they do not fill the blank production-candidate rows below.

The [older production 0.1.0 image](../build/release-candidate-refresh-20261026/README.md)
(`7bdc…d007`) has scoped X4 normal-reading/interface evidence with Korean
TOC/bookmark glyph defects retained in its [audit](../build/x4-production-automation-20261026/independent-validation/production-evidence-through-reader-features-v1.json).
Its later [corrected external-RTC trial](../build/x4-production-resume-20261026/independent-validation/early-sleep-independent-audit.json) established ordinary wake and normal
reopen, not GPIO wake/current/timing acceptance. Its normal SD invalid/Cancel/
Confirm trial and [original full16MiB/dev boot/known-SD restoration](../build/x4-production-resume-20261026/original-restoration-closure.json)
completed before the new trial. Earlier late-breakpoint, generic-key and
transport failures remain preserved; this historical receipt alone does not
establish the current device state.

The [latest overlay candidate](../build/reader-overlay-ui-font-rc-20261026/receipt.json)
is production `0.1.0`, SHA
`a085c394b18829e5aac15f44184b0b96c97983fc8b049568415ea38ae773c99d`.
Its prior R3 [exact app1 USB trial](../build/x4-production-continuity-20261026/independent-install-audit.json) and scoped restoration completed. R4 exact installation/readback and injected RTC wake/normal reopen passed as qualified below; final public/preflash scope passed, original16MiB ROM restore, wd/X4/app1 startup and postboot known-state/clean Sync closure completed within the explicit accepted6 scope. [Final closure](../build/production-sleep-x3-20261027/restoration-closure.json) and [independent audit](../build/production-sleep-x3-20261027/independent-validation/final-restoration-review.json).
Native15/ASAN15, six mutants, full host1071 and strict cppcheck passed; default
and gh_release builds had zero warnings. The native host build retained 45
warning lines outside the changed nine files. The matching dev `…wb05eda4a`
installed on X3 with known-state preservation and an authorized update.bin
exception; Library KO48/EN56/cleanup passed, but reader overlays are unverified.

Latest X4 [EN120](../build/reader-overlay-ui-font-rc-20261026/x4/chapter-ocr-audits/en-final-001/independent-full-book-audit.json)
and [KO98](../build/reader-overlay-ui-font-rc-20261026/x4/chapter-ocr-audits/ko-final-001/independent-full-book-audit.json)
passed normal page progression with 218 saved RAM frames, 48 chapter-start OCR
checks and source PNG geometry. [KO Menu/TOC, footnote and bookmark navigation](../build/reader-overlay-ui-font-rc-20261026/x4/overlay-audits)
passed separately. These are scoped evidence, not optical/full-text coverage
or final production acceptance.

**Normal SD and continuity evidence:** SD V2 observed invalid-file rejection,
default Cancel and a normal confirmed install, followed by a fresh boot and
[cached running-app0 observation](../build/x4-production-continuity-20261026/normal-sd-trial-002/running-app0/receipt.json).
The later full-flash classifier confirmed exact app0/app1/OTA state before
restoring the original flash. V1's34hex upload nonce was rejected locally before
curl; its consumed marker and failed journal remain preserved. V2 used fresh
markers and exact32hex temporary names without weakening HTTP validation.
[SD2 known-state cleanup](../build/x4-production-continuity-20261026/normal-sd-trial-002/independent-post-sd-closure-review.json)
preceded the new continuity run.
This covers the observed invalid BIN, Cancel and Confirm/install path, not all
malformed images, SD failures or production cache recovery. Old7bd cache-clear
evidence is not promoted to latest-candidate acceptance.

Corrected Mac production continuity2702 passed one bidirectional round with three
native captures, normal reader Confirm, app Go, no automatic movement before
acceptance and accepted-text visibility. Requested anchor and actual page-start
pointer are recorded separately. The [actual host audit](../build/x4-production-continuity-20261026/continuity-2026102702/independent-host-audit/receipt.json)
binds the real XCTest PASS and saved app/frame evidence. Its [step4 restoration](../build/x4-production-continuity-20261026/continuity-2026102702/independent-restoration-review.json)
verified raw6, original5 books, generated/temp/cache absence and public reading.
Strict inventory was false only for existing root-log growth410B, with separate
scoped success. Run2701's foreground-resume timeout remains failed evidence.
The corrected isolated host was tested before its two HardwareTests files were
[applied to the app checkout](../../pocket-daily/.build/production-continuity-correction-isolated-20261027/actual-application-20261027/receipt.json).
All191 shipping inputs and signed IPA/PKG remain unchanged; those Store artifacts
are not the tested hardware host.

**Current R4 controls and new-trial boundary:** [original-control closure](../build/production-sleep-x3-20261027/x4-original-controls-preservation-closure.json) completed before fresh production flash.
Original wd demonstrated awake watchdog reset SDK7/raw16, two normal10min RTC
wakes SDK8/raw5/wake8, and separate no-clear V2 watchdog reset SDK7/raw16/wake0
after normal sleep commit. The last result does not prove exact firing inside
deep sleep or rescue independent of all power/clock failures. Terminal reply
unknown/exit1, old observer gaps and missing preattach whole snapshot remain.
These original-wd results do not sign off production a085 sleep.

R4 raw5, original books/RC2, crash4, sleep-frame absence and known temp paths were
preserved. Original-book opening advanced global sequence5→6 and per-book
progress/CRC record. The coherent actual record is retained because
original cache bytes/native page were not backed up. rawSixStrictOriginal,
publicReadingStrictOriginal and fullSDRestored remain false. Do not delete or
reconstruct unknown cache to force a strict result.

The [scoped public audit](../build/production-sleep-x3-20261027/original-controls-final-public-scoped-001/receipt.json)
accepts only that one exact reading row; all other public values and fixed three
folders are preserved. Profile132/content199 are unchanged, existing root log
increased302B. Raw/cache/books checks followed normal reload/Library and preceded
one clean Sync; final public followed its ACK and debugger shutdown. This is
known-scope preservation with an explicit metadata exception, not full SD.

The [prior R3 restoration](../build/x4-production-continuity-20261026/restoration-closure.json)
proved original16MiB at final ROM readback, normal wd/app1 boot, then original
raw6/known SD. It is the pre-R4 baseline, not proof of the current later state.
[R4 exact install](../build/production-sleep-x3-20261027/independent-validation/install-a085-review.json) passed full readback.
[Actual production sleep/reopen](../build/production-sleep-x3-20261027/x4/production-reader-trial/independent-sleep-audit/final-sleep-and-reopen-audit.json) passed normal600051ms,
externally injected RTC3s, SDK8/raw5/mask8 and PocketDaily shell startup. Normal
same-owned-file reopen matched spine0/page3/offset1262 and all48,000 frame bytes.
Automatic reader resume=false: injected TIMER maps to Other/isSleepWake=false;
this is not physical PowerButton quick-resume or production self-scheduling proof.
The800s observer remained healthy, but root attached after61s stable serial;
whole preattach copy exists and later registry cannot prove untouched stability.
Terminal unknown/exit1 is retained. The initial a085 UNKNOWN stays historical.

[R4 cleanup](../build/production-sleep-x3-20261027/x4/production-reader-trial/independent-cleanup-review.json) restored state/recent/global7→6 and
verified finalraw6/freshbefore, accepted original cache6, crash4/books5 and owned/
bookmark/sleep absences after normal reload/Library. [Final public/preflash scope](../build/production-sleep-x3-20261027/x4/production-reader-trial/preflash-scoped-preservation-review.json) passed:
accepted reading/other values/three folders equal, existing log+154B vs controls
(+456B vs original R4 pretrial), generations132/199 unchanged. [R4 original flash restore](../build/production-sleep-x3-20261027/independent-validation/restore-original-review.json) completed one app1 write and exact original16,777,216B final ROM readback.
[Separate boot](../build/production-sleep-x3-20261027/boot-original-001/receipt.private.json) observed wd/X4/app1, reset ACK and no panic pattern. Flash equality is preboot; the helper's generic boot acceptance remains false.
[Final closure](../build/production-sleep-x3-20261027/restoration-closure.json) and [independent audit](../build/production-sleep-x3-20261027/independent-validation/final-restoration-review.json) completed postboot known-state and clean Sync verification.
[Raw6](../build/production-sleep-x3-20261027/final-original-raw-six-comparison.json) equals accepted preproduction global6; cache2+4absent, original crash4, sleep absence, two owned bookmarks absent twice and five original books2,462,328B were verified.
[One clean Sync](../build/production-sleep-x3-20261027/final-original-clean-dev-sync-001/receipt.json) ACK preceded [public002](../build/production-sleep-x3-20261027/final-original-public-scoped-002/receipt.json) scoped PASS: accepted reading6 row/other values/fixed3folders retained; profile132/content199 equal, existing log+558B vs pretrial.
The first public001 connection failure and strict comparison remain saved. Raw/cache/book checks preceded the final Sync restart. R4 closed with original wd Same Wi-Fi Sync and its owned workers/assertion stopped;
R5 active state supersedes that historical observation.
The a085-rtc-first case and marker are consumed; never reuse them. FullSD=false remains.

R5 supersedes the X3 transport/return gap: the [V3 X3 trial](../build/wifi-trial-recovery-20261027/x3-observe-002/receipt.json)
observed exact new production SHA `6b9277b6…73137d`, public state, 1,184B transfer/download
and automatic Returned/failure0/SW-reset Same Wi-Fi recovery. The full actual
7,798,784B return slot was independently exported first; X3's actual table differs
from X4 and is never rewritten. [Contract](developer-trial-lease.md).
This is boot/HTTP/return evidence, not production page input/quick-resume/SD recovery.
[X3 known-state closure](../build/wifi-trial-recovery-20261027/x3-final-known-001/receipt.json)
verified original wb05, public reading/settings, raw state/recent/settings/dirty and
16 unchanged supported files3,062,182B (10 EPUB/6 TXT, including diagnostics). Index nextFirstSeen25→26 is the only index-byte difference;
original per-book cache/fullSD equality is not claimed. The [same V3 image on X4](../build/wifi-trial-recovery-20261027/x4-observe-003/receipt.json)
also passed boot/1,216B transfer/timed return; V2 X4 is a separate historical artifact.
Developer20-page Library/cleanup passes do not establish production reading or
physical PowerButton resume. X4's [final V3 restoration](../build/wifi-trial-recovery-20261027/restoration-closure.json)
closed original16MiB-before-boot and postboot public/raw/book scopes; current handoff owns the
recorded index/log/cache exceptions and remaining gates.

Latest a085 reading/SD/continuity passes above remain valid within their recorded
scopes. Release remains on hold. Consume only fresh reviewed plans/outputs;
never replay an unknown arm or reuse a consumed marker. The bounded production
supervisor requires same-server live proof; it does not itself classify wake.
[Current handoff](../../pocket-daily/docs/CURRENT_HANDOFF.md) owns active state.
No upload, tag, release, commit or push is authorized by this checklist.

For **0.x**, complete all automated gates, the model-specific update/recovery
gates, the minimum device pass and publication checks. X3 supports companion
download and SD staging with on-reader confirmation; reader-native HTTPS
checking/downloading is explicitly unsupported with the current TLS memory
budget. Record that limitation separately from the supported-path result.
X4 must pass its native HTTPS updater as well as the companion path.
Candidate installation, safety and recovery checks are required before the
tag. Checks that require the newly published release are marked below and
remain pending until that endpoint exists; publication alone is not final
release sign-off.

For **1.0.0**, complete the full hardware and feature sections on both models.
The X3 native-HTTPS limitation must be resolved and tested, or its continued
exclusion must receive an explicit product-scope decision and matching public
documentation before stable sign-off. This 0.x scope does not grant that
future approval. No change here signs off an image or a physical scenario.

## Candidate identity

- Version/tag:
- Git commit:
- `firmware.bin` SHA-256:
- Build environment (`gh_release` for a normal release):
- App commit/build and tested platform:
- Tester/date:

## Automated gates

- [ ] `./bin/clang-format-fix` leaves no diff.
- [ ] Strict `./scripts/pio.sh check` passes with no defects.
- [ ] `./scripts/pio.sh run -e default` succeeds.
- [ ] Production `./scripts/pio.sh run -e gh_release` succeeds.
- [ ] Production image reports exactly `platformio.ini`'s version; developer
      flash/reset/capture/diagnostic routes are absent. A `default` or
      `ble_standby` image cannot substitute for production acceptance.
- [ ] Complete host test suite passes.
- [ ] Pull-request CI passes.
- [ ] `main` CI passes after merge.
- [ ] App-side Nearby Sync protocol tests pass for the same contract.

## Model-specific update and recovery gates (0.x and 1.0.0)

Record the app build, firmware commit/hash, tester/date and result for each
model. Perform the supported update route on the exact production image.
Keep installation evidence distinct from transport completion.

### Both X3 and X4: companion and SD route

- [ ] The app retrieves a supported official release, validates the image and
      embedded version, and stages it over Same Wi-Fi. Also exercise Direct
      with the image downloaded before joining the reader's private network.
      Before first publication, Debug-only **Send a Local Build…** can stage
      the exact candidate for device validation. This does not sign off the
      shipping app, which has no local firmware import. Leave its official
      release discovery/download/staging flow pending until publication and
      verify it with a Release app build.
- [ ] Independently compare the staged SD file's whole-file SHA-256 with the
      candidate hash using file readback or mounted-SD inspection. A successful
      upload, receipt or byte count alone does not prove this check.
- [ ] Finish the transfer session and leave the transfer screen. The reader
      requests its own installation confirmation; cancel leaves the existing
      firmware installed, and staging never initiates flashing automatically.
- [ ] Confirm on the reader, then reconnect and match the same reader identity
      and exact installed version. Missing identity or failed reconnection is
      unresolved installation status, not automatic confirmation.
- [ ] Reject truncated, incompatible/wrong-board or invalid images without
      installing them. Exercise transfer interruption and cancellation, then
      demonstrate recovery to another transfer and normal reading.
- [ ] Restore a previous compatible image using SD recovery with `update.bin`;
      verify boot, saved books/settings and reading-position behavior.
- [ ] Record pre/min/post heap, largest block, crash/watchdog evidence and any
      recovery steps. A low-memory refusal must remain responsive and recover
      without silently lowering memory thresholds.

### X3: current native-HTTPS limitation

- [ ] Record reader-native HTTPS update checking/downloading as
      **unsupported — TLS memory limit**, not passed or untested by implication.
      Exercise the memory-failure message, companion guidance and Back/retry
      recovery; verify no crash or unintended installation.
- [ ] Complete the companion/SD gates above on X3. Release notes and support
      instructions state this model-specific limitation and usable update path.

### X4: native HTTPS updater

- [ ] After publication, start from a previous compatible version and verify
      certificate-verified release checking and streamed OTA into the published
      candidate, with separate reader confirmation. Match its SHA-256 to the
      pre-tag candidate before this run.
      Cancel, interrupted download and invalid-image paths preserve a bootable
      reader; subsequent retry and SD recovery work.
- [ ] Network/server/memory errors show the appropriate failure and recovery
      guidance. A memory refusal is not a successful update; retry after
      recovery must demonstrate the supported path without a crash.
- [ ] After publication, **Sync → Check for updates** on that released version
      reports no update and no error. An older or excluded GitHub latest tag
      cannot establish this publication check.

| Model | Commit + image SHA-256 / app build | Supported update path / evidence | Native HTTPS status |
| --- | --- | --- | --- |
| X3 | Production pending; new dev `…wb05eda4a` install/Library regression PASS | New production update/recovery untested | Unsupported in current 0.x scope; failure/recovery acceptance pending |
| X4 | a085 reading/SD/continuity plus R4 injected RTC/normal reopen PASS; automatic reader resume=false | R4 cleanup/public scoped PASS/global6 baseline; original16MiB/startup observed, postboot known-state/clean Sync closure complete, accepted6 exception/fullSD=false | Required; pending |

## X3 hardware

- [ ] Installed the candidate identified above.
- [ ] BLE entry retains at least 20 KiB minimum-ever free heap.
- [ ] Post-init pairing gate has at least 20 KiB free and an 8 KiB largest block.
- [ ] BLE teardown leaves enough contiguous heap for private AP and HTTP.
- [ ] Pairing, hotspot handoff, identity verification, streamed upload, verified
      commit, crash-report retrieval, Back cancellation, and timeout recovery pass.
- [ ] Firmware staging does not flash automatically; reader confirmation works.
- [ ] SD recovery with `update.bin` succeeds.

Evidence/log reference:

## X4 hardware

- [ ] Installed the candidate identified above.
- [ ] BLE entry retains at least 20 KiB minimum-ever free heap.
- [ ] Post-init pairing gate has at least 20 KiB free and an 8 KiB largest block.
- [ ] BLE teardown leaves enough contiguous heap for full AP/HTTP profile; the
      pre-HTTP gate has at least 20 KiB free and a 10 KiB largest block.
- [ ] Pairing, hotspot handoff, identity verification, streamed upload, verified
      commit, crash-report retrieval, Back cancellation, and timeout recovery pass.
- [ ] Firmware staging does not flash automatically; reader confirmation works.
- [ ] SD recovery with `update.bin` succeeds.

Evidence/log reference:

## Major upstream baseline upgrade (both X3 and X4)

Record results separately for each device against the candidate identity above.
Host render tests do not sign off panel or radio behavior.

- [ ] Upgrade from the previous product build with existing settings and books;
      legacy 0..3 app font sizes still select the expected point sizes.
- [ ] Old complete and partial section caches regenerate as v136; opening,
      page turning and resuming do not reset the accepted reading position.
- [ ] Reading-position round trips between the companion and reader land on
      the same text, including entities, long paragraphs, hyphenation and CJK.
- [ ] Built-in and SD fonts, character/word spacing, footnotes,
      RTL and grayscale images work in all four orientations.
- [ ] Classic, Lyra, Lyra 3 Covers and RoundedRaff draw stock CrossPoint menus
      and button hints (two-line labels), Korean labels render, and an
      OPDS-configured Home pages with page marks instead of covering the hints.
- [ ] Home exposes one Pocket Daily entry; Articles opens from Pocket Daily.
- [ ] Pocket Daily's first button reads Home and opens CrossPoint Home.
- [ ] A book opened from Pocket Daily returns there on Back; an article returns to
      Articles; a book opened from Home returns to Home.
- [ ] With "Back goes to the file browser" on: Pocket Daily book → Back to the file
      browser → open another book → Back returns Home, not Pocket Daily.
- [ ] Page turning, background layout and repeated book/font changes retain
      over 50 KiB free heap in the reading acceptance scenario without leaks;
      record free heap, largest block and minimum-ever heap separately.
- [ ] Repeat the device-specific radio/transfer gates above after a reading
      session; record heap before/during/after and exercise Back cancellation.
- [ ] Repeat the model-specific update/recovery gates after the reading
      session. X4 passes certificate-verified HTTPS and streamed OTA; X3 passes
      the companion/SD path and records its native-HTTPS limitation. For
      `1.0.0`, apply the additional scope decision above.
- [ ] Validate saved-network reconnect, sleep/wake, progress persistence and
      watchdog recovery using the new FreeInk SDK on the physical device.

| Device | Tested commit + image SHA-256 | Result / evidence |
| --- | --- | --- |
| X3 | Pending | Not tested |
| X4 | Pending | Not tested |

## Automatic BLE reading-sync candidate (both X3 and X4)

- [ ] Complete `docs/ble-sync-review-2026-09-30.md` with actual list/offer completion,
      not advertising alone; record the app build and firmware image identity.
- [ ] Verify book-close (back to Pocket Daily), wake and sleep windows, and that
      no window opens on Home, Library or the file browser; memory skips,
      early close during startup, safe radio handoff and deinit recovery.
- [ ] Verify existing-bond reconnect including RPA, unbonded refusal, no new
      pairing or START_AP in an automatic window, app foreground/background,
      explicit Nearby Sync priority, demo, toggle off and forget.
- [ ] Measure pre/ready/min/post free heap and largest block on the current SDK;
      do not reduce gates to hide missing headroom. Mark skipped-only scenarios
      as unavailable, not successful reading sync.

## Reader file download (both X3 and X4)

Implemented in PR #13; hardware acceptance is still pending. Record app build,
firmware commit and image SHA-256. Run on Same Wi-Fi and Direct separately.
See [reader-files.md](reader-files.md) for the contract.

- [ ] Download 1 MB and 20 MB EPUBs and representative TXT/MD files; compare
      whole-file SHA-256 with the SD originals, not only received length.
- [ ] Confirm `readerFiles: 2`, exact Content-Length, no-store and piece caps
      (4096 / 1024 bytes), including the final short piece.
- [ ] Exercise malformed query, forbidden path/type, missing file, wrong
      device ID, changed size, EOF offset, busy and low-heap responses;
      verify retry/backoff, restart and cancellation with the app.
- [ ] Interrupt and resume; confirm no duplicate/missing bytes and that cancel
      leaves the reader responsive for reading and another sync session.
- [ ] Record throughput and pre/min/post heap, largest block and watchdog/crash
      results. Host reassembly tests do not establish these device results.

## Companion connection changes — 2026-10-05

Pending separately on **X3 and X4**, against the exact app revision and firmware
artifact hash. No host/simulator result signs these off.

- [ ] Same Wi-Fi and Direct: publish an EPUB; drop the commit response, reconnect
      and explicitly resume. Match the durable receipt without another upload;
      restart reader and app and repeat. Verify exact file bytes on SD.
- [ ] Lose power before receipt persistence or replace/remove the receipt: the app
      must report unknown and preserve its queue, not resend or claim success.
- [ ] Download official firmware before Direct connection, then Send update.
      Verify SD staging, successful session/end on both profiles, on-reader
      installation/cancel confirmation and installed version on a later reconnect.
- [ ] BLE: pair without READ1, pair with READ1, close a book, background the app,
      sleep/wake, force a memory/battery gate and use HTTP diagnostics. Check the
      app distinguishes waiting, unavailable, failure and completed exchange.
- [ ] Reading: observe the reader, reread an earlier position in the app, exchange,
      accept/cancel on the reader. Cancel must not repeat the same suggestion.
      Change reader position before delivery/opening: reject the stale offer.
- [ ] Interrupt BLE with Same Wi-Fi or Direct setup; cancel and retry. Record heap,
      largest block, watchdog/crash and radio teardown; exercise X3/X4 controls
      and all four orientations. No new claim that BLE is always available.

## Publication

- [ ] Nearby Sync v1 is frozen and compatible with the app repository.
- [ ] Release notes describe compatibility, recovery, and known limitations.
- [ ] Owner authorizes the exact `pocket-v<version>` tag and release; the tag
      points to the signed-off commit. A release tag publishes automatically.
- [ ] GitHub release contains `firmware.bin`, `firmware.elf`, `firmware.map`,
      `bootloader.bin`, `partitions.bin`, and license notices.
- [ ] Published `firmware.bin` SHA-256 matches the tested candidate; otherwise
      the new image remains unapproved and requires exact-image acceptance.
- [ ] `/releases/latest` resolves to the intended `pocket-v<version>` and the
      companion stable channel parses/downloads it. Complete the post-publication
      X4 no-update check; X3 remains on the documented companion/SD path.

## Development (0.x) releases

Versions below 1.0 are published as normal releases (`pocket-v0.x.y`, marked
latest) so the reader's updater and the companion's stable channel find them;
see `docs/product-versioning.md`. They reach every lineage 2 reader, so the
automated gates above apply in full and the exact release image gets at least
this device pass on an X3 and an X4 before the tag (record commit, image
SHA-256, tester/date under Candidate identity):

- [ ] Installs through the SD updater and boots to Pocket Daily; About shows
      the version.
- [ ] Opens a Korean and an English EPUB, turns 20 pages, sleeps and wakes.
- [ ] Korean and English metadata titles are readable in the actual Library
      list, and selecting each row opens the matching book. Title strings or
      row counters alone do not establish visible glyphs.
- [ ] A known EPUB image is visible after cold indexing and again from cache;
      neither successful page counts nor an image block with a placeholder
      establishes successful image painting. Preserve the captured evidence.
- [ ] Home, Library, Settings and File Transfer open; labels are readable in
      the configured language on the selected theme.
- [ ] Sync with the companion: pairing or Same Wi-Fi, one upload, reading
      position exchange.
- [ ] Complete the pre-tag model-specific update/recovery gates above. X3's
      native HTTPS limitation stays visible. Record X4's official OTA and
      no-update checks separately after publication; they remain required for
      release sign-off.
- [ ] No crash report; `/api/status` `freeHeap` above 50 KB on Home.

The full X3/X4 and feature sections remain the gate for `1.0.0`, subject to the
explicit X3 support-scope decision above. Unchecked full-suite items are not
claims of coverage by the smaller 0.x pass.

## Beta pre-releases (companion beta channel)

Tag `pocket-v<version>-beta.<n>` (for example `pocket-v0.2.0-beta.1`, where `<version>` is
`platformio.ini`'s) to publish a GitHub **pre-release**. The release workflow
builds `gh_release_beta`, checks that `firmware.bin` reports exactly
`<version>-beta.<n>`, and publishes with `--prerelease --latest=false`, so
`/releases/latest` (store builds of the app, the reader's own updater) never
sees it. Development builds of the app list recent releases, pre-releases
included, and offer the newest one through Update reader. A beta is for
hardware testing: this checklist still gates the stable tag. Any other tag
shape fails the workflow before building.
