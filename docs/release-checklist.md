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

## Publication

- [ ] Nearby Sync v1 is frozen and compatible with the app repository.
- [ ] Release notes describe compatibility, recovery, and known limitations.
- [ ] Tag points to the signed-off commit.
- [ ] GitHub release contains `firmware.bin`, `firmware.elf`, `firmware.map`,
      `bootloader.bin`, `partitions.bin`, and license notices.
- [ ] `/releases/latest` resolves and the on-device updater parses the release.

## Beta pre-releases (companion beta channel)

Tag `v<version>-beta.<n>` (for example `v1.7.0-beta.1`, where `<version>` is
`platformio.ini`'s) to publish a GitHub **pre-release**. The release workflow
builds `gh_release_beta`, checks that `firmware.bin` reports exactly
`<version>-beta.<n>`, and publishes with `--prerelease --latest=false`, so
`/releases/latest` (store builds of the app, the reader's own updater) never
sees it. Development builds of the app list recent releases, pre-releases
included, and offer the newest one through Update reader. A beta is for
hardware testing: this checklist still gates the stable tag. Any other tag
shape fails the workflow before building.

