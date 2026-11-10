# Firmware 1.0.0 candidate — 2026-11-01

First release uses standard EPUB and lineage 2, with release and developer
builds as specified in [product versioning](product-versioning.md).

## Local security preparation — 2026-11-04

The security candidate was built at8dc08b1c with changes subsequently committed
as main/origin dbd99730. Its sealed security files match that tree. It differs from the historical
November1 candidate below: gh_release1.0.0, 6,386,624 bytes,
SHA256 `d2aeaed30fd6009114afa58b823ab4c051c3630d34e0ce53ab660a35e606f53e`. Frozen copy and source/frame/build evidence:
`build/release-security-20261104/verification.json` and `production.bin`.
Host1194/default+gh_release warning0/strict cppcheck pass. **No hardware
installation or acceptance, tag or firmware publication.** Source is pushed;
CI41827596371 passed. **No final release acceptance.** All applicable exact-image
X3/X4 gates remain open; this does not inherit November1 or older sign-off.
See [HTTP origin safety](http-credential-origin.md).

## Owner installation preparation — 2026-11-05

Owner explicitly requested persistent submission-candidate installation for personal
use validation. Both enrolled X3/X4 identities, models and previous wdd553a3b
versions verified. Entire active app slots match the reviewed old artifact prefix;
known raw settings/profiles/library/reading files and prior /update.bin preserved.
Private evidence: `build/owner-install-20261105/` (not whole SD/NVS equality).

Both X3/X4 /update.bin whole readbacks match the exact production SHA above.
X4 publication completed through Sync in the initial preparation. After the owner
resumed all devices, X3's preserved staging path returned validated resume offset0;
full size/CRC, one-shot atomic publication and entire File Transfer readback passed.
Its reading records had changed, so fresh known raw-state/reading/prior-update
backups were completed first. Successful receipts:
`build/owner-install-20261105/X3-resumed-current-backup2/`.
The initial interrupted X3 upload and local guard stops remain private evidence;
no unknown commit was replayed. The updated app production harness admits exact
1.0.0 and keeps identity/model/registry gates; Mac13/iOS1 deterministic checks pass.

Native X3 installation is requested, not verified. X4 owner did not observe update
progress/restart and requested another attempt; fresh same-identity status still
reports developer wdd553a3b on both readers. No serial USB interface is currently
enumerated, and the supported developer --flash clients reject production images.
The native SD updater's final confirmation remains required; no guard bypass,
remote flash, trial lease, release tag or publication was performed.
Staging and harness checks are not installed-image hardware acceptance.

## Historical November1 frozen image

- Source commit `6025da03` includes the content-sized Home cover layout and
  native/host preview comparisons.
- Environment `gh_release`, version exactly `1.0.0`, 6,374,848 bytes.
- SHA-256 `4376a1f99da9d8d4e6369a882a7038436c6889a4ef5857d76d027e6e4ed84d13`.
- Local artifacts and logs in `build/release-prep-20261101/`.
- Developer bridge images on the two readers have different versions and
  hashes. Their earlier acceptance does not establish this image's coverage.

## Automated validation

Host tests 1,156/1,156 and Python tests 310/310 passed. Strict cppcheck 2.11,
`default` and `gh_release` builds passed. Local format verification passed.
CI and exact-image device checks are recorded below as they complete.

## Device validation

X3 completed the image-bound production Home trial. It captured native Home
with the book cover and English/Korean title, measured free heap 75,244B and
largest block 61,428B, then returned to
`1.0.0-dev-main-83eb0443-w6777cde0`. The whole active return slot was backed up
and verified before the trial. State, recents, settings, library index and dirty
marker all matched the pretrial raw bytes. The original staged update was
restored, the owned frame exported and removed, and the terminal lease cleared.
Receipt `build/release-prep-20261101/x3/home-finish/receipt.json` records the scope.
The captured frame is digital framebuffer evidence.

X4 known raw/public state and the whole active return slot were backed up, but
its first production-stage command timed out during the read-only status check
with `networkMutationIntents=0`. No production image was transferred or started
on X4. The app's enrolled-reader discovery found no matching result before it
was stopped. X4 needs to be prepared in Same Wi-Fi to continue. Preserve the
failed stage output and recheck identity, exact original version, raw state and
any pending lease before creating a new output for the next attempt.

## Release gates

The [release checklist](release-checklist.md) still requires both models'
reading, sleep/wake, transfer, pairing, update/recovery and feature scenarios
on this exact image. Native HTTPS remains excluded for X3 by the documented
1.0.0 product scope, and required for X4. Physical checks retain their recorded
scope and exclusions.

Historical X3 File Transfer settings abort evidence is from
`0.1.0-dev-fix-honor-chosen-reader-font-2b7598ce-w7ded7a3e`. Current-image
reproduction remains open. X4's one historical missing-reading-record event
also remains unresolved. Both need explicit disposition for release sign-off.

Create `pocket-v1.0.0` only after exact-image acceptance, green main CI and
owner approval of the tagged commit and published artifact. Update the app's
`FirmwareGuidance.minimumRecommended` together with that publication.

## Publishing the stable image

Download `production-release-assets` from successful push CI for the release
commit on `main`. Install and verify that exact `firmware.bin` on both readers
and record its SHA-256 with the hardware results. Create the annotated stable
tag with the subject `Firmware-SHA256: <lowercase SHA-256>`. The tag workflow
publishes those same CI assets after checking the checksum and embedded version.
