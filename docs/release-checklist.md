# Pocket Daily Firmware Release Checklist

Complete this checklist for the exact commit and artifact that will be tagged.
Do not infer hardware results from host checks.

## Candidate identity

- Version/tag:
- Git commit:
- `firmware.bin` SHA-256:
- Tester/date:

## Automated gates

- [ ] `./bin/clang-format-fix` leaves no diff.
- [ ] Strict `./scripts/pio.sh check` passes with no defects.
- [ ] `./scripts/pio.sh run -e default` succeeds.
- [ ] Production `./scripts/pio.sh run -e gh_release` succeeds.
- [ ] Complete host test suite passes.
- [ ] Pull-request CI passes.
- [ ] `main` CI passes after merge.
- [ ] App-side Nearby Sync protocol tests pass for the same contract.

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
- [ ] Old complete and partial section caches regenerate as v134; opening,
      page turning and resuming do not reset the accepted reading position.
- [ ] Reading-position round trips between the companion and reader land on
      the same text, including entities, long paragraphs, hyphenation and CJK.
- [ ] Built-in and SD fonts, character/word spacing, bilingual pages, footnotes,
      RTL, grayscale images and UI packs work in all four orientations.
- [ ] Home exposes one Pocket Reader entry; Articles opens from that entry.
- [ ] Page turning, background layout and repeated book/font changes retain
      over 50 KiB free heap in the reading acceptance scenario without leaks;
      record free heap, largest block and minimum-ever heap separately.
- [ ] Repeat the device-specific radio/transfer gates above after a reading
      session; record heap before/during/after and exercise Back cancellation.
- [ ] Certificate-verified HTTPS and streamed OTA succeed; truncated/wrong-board
      images are rejected and SD recovery with the previous product image works.
- [ ] Validate saved-network reconnect, sleep/wake, progress persistence and
      watchdog recovery using the new FreeInk SDK on the physical device.

| Device | Tested commit + image SHA-256 | Result / evidence |
| --- | --- | --- |
| X3 | Pending | Not tested |
| X4 | Pending | Not tested |

## Automatic BLE reading-sync candidate (both X3 and X4)

- [ ] Complete `docs/ble-sync-review-2026-09-30.md` with actual list/offer completion,
      not advertising alone; record the app build and firmware image identity.
- [ ] Verify book-close/Home/Library/pop, wake and sleep windows; memory skips,
      early close during startup, safe radio handoff and deinit recovery.
- [ ] Verify existing-bond reconnect including RPA, unbonded refusal, no new
      pairing or START_AP in an automatic window, app foreground/background,
      explicit Nearby Sync priority, demo, toggle off and forget.
- [ ] Measure pre/ready/min/post free heap and largest block on the current SDK;
      do not reduce gates to hide missing headroom. Mark skipped-only scenarios
      as unavailable, not successful reading sync.

## Publication

- [ ] Nearby Sync v1 is frozen and compatible with the app repository.
- [ ] Release notes describe compatibility, recovery, and known limitations.
- [ ] Tag points to the signed-off commit.
- [ ] GitHub release contains `firmware.bin`, `firmware.elf`, `firmware.map`,
      `bootloader.bin`, `partitions.bin`, and license notices.
- [ ] `/releases/latest` resolves and the on-device updater parses the release.

## Beta pre-releases (companion beta channel)

Tag `pocket-v<version>-beta.<n>` (for example `pocket-v1.0.0-beta.1`, where `<version>` is
`platformio.ini`'s) to publish a GitHub **pre-release**. The release workflow
builds `gh_release_beta`, checks that `firmware.bin` reports exactly
`<version>-beta.<n>`, and publishes with `--prerelease --latest=false`, so
`/releases/latest` (store builds of the app, the reader's own updater) never
sees it. Development builds of the app list recent releases, pre-releases
included, and offer the newest one through Update reader. A beta is for
hardware testing: this checklist still gates the stable tag. Any other tag
shape fails the workflow before building.
