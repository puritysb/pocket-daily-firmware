# Firmware 1.0.0 candidate — 2026-11-01

First release uses standard EPUB and lineage 2, with release and developer
builds as specified in [product versioning](product-versioning.md).

## Frozen image

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
