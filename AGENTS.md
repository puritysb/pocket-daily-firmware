# Pocket Daily Firmware Agent Guide

This is the shared entry point for coding agents in the firmware repository.
The detailed embedded constraints live in `CLAUDE.md` (a link to
`.skills/SKILL.md`). Durable repository and release context lives in
`docs/PROJECT_MEMORY.md`.

## Read in this order

1. Read this file for workflow and verification requirements.
2. Read `CLAUDE.md` before changing firmware code or hardware behavior.
3. Read only the task-relevant source, tests, and focused skill under
   `.claude/skills/`.
4. Read `docs/PROJECT_MEMORY.md` for repository boundaries and durable state.
5. Read `docs/release-checklist.md` before declaring a release complete.

## Repository identity and boundaries

- Repository: `puritysb/pocket-daily-firmware`
- Default product branch: `main`
- Foundation remote: `upstream` → `crosspoint-reader/crosspoint-reader`
- App repository: sibling `/Users/puritysb/github/pocket-daily`

This repository owns reader behavior, device endpoints, on-device validation,
and flashing. The app repository owns iOS/iPadOS/macOS code and App Store
assets. Bluetooth records, endpoint payloads, file layouts, and update rules
are cross-repository contracts; verify both sides when any of them changes.

Pocket Daily product work stays on `main`. Upstream CrossPoint sync remains a
controlled merge through `scripts/sync-upstream.sh`; never rebase product
history onto upstream and never send Pocket Daily product-stack files upstream.

## Native PlatformIO wrapper

Use `./scripts/pio.sh`, not a bare `pio` command. The wrapper selects the native
arm64 Homebrew installation on Apple Silicon and avoids loading an x86_64
PlatformIO interpreter with arm64 Python extensions. Set `PLATFORMIO_BIN` only
when an explicit executable override is required.

```sh
./scripts/pio.sh run -e default
./scripts/pio.sh check --fail-on-defect low --fail-on-defect medium --fail-on-defect high
```

Every successful build stages an ignored `firmware/update.bin`. Read
`firmware/LATEST_BUILD.txt` before identifying or installing the artifact.

## Host tests

Use a path-independent ignored build directory so repository moves do not
reuse a stale absolute CMake cache:

```sh
cmake -S test -B build/host-tests -DCMAKE_BUILD_TYPE=Release
cmake --build build/host-tests
ctest --test-dir build/host-tests --output-on-failure -j
```

## Required verification

- C/C++ or build changes: format, strict cppcheck, default firmware build, and
  the complete host test suite.
- Protocol changes: add deterministic host tests and check the app-side
  implementation in `pocket-daily`.
- Generated content: edit its source, run the relevant generator, and follow
  `.claude/skills/generated-content/SKILL.md`.
- Release changes: validate `gh_release`, obtain X3 and X4 hardware sign-off,
  merge with green CI, then create the release tag.

Run formatting with `./bin/clang-format-fix`, then inspect the diff. Do not
silence cppcheck broadly. A narrow suppression must explain the embedded-system
reason and preserve meaningful defect detection.

## Hardware and memory safety

The ESP32-C3 has no PSRAM. Runtime heap and the largest contiguous block are
release constraints. Follow `CLAUDE.md` for allocation, HAL, storage locking,
task, cache, and UI rules. Do not claim Bluetooth, Wi-Fi handoff, local HTTP,
SD-card, OTA, recovery, watchdog, or heap behavior from a host build.

Use `.claude/skills/firmware-deploy/SKILL.md` for deployment. An agent may stage
an artifact, but the user installs it and confirms physical-device results.

## Git and release hygiene

- Inspect `git status`, the current branch, and remotes before Git operations.
- Preserve unrelated changes and never commit ignored build output.
- Do not commit secrets, Wi-Fi credentials, passkeys, device identifiers, or
  private serial logs.
- CI must pass on `main` before tagging.
- A release tag publishes the production `gh_release` artifact. Never tag or
  publish before both hardware rows in `docs/release-checklist.md` are signed
  off with the tested commit and artifact hash.
