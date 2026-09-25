# Pocket Daily Firmware Agent Guide

This is the only project instruction file for every coding agent working in
this repository — Claude Code, Codex, OpenCode, the GitHub coding agent, and
any model they drive (GLM included). It holds the always-applicable embedded
rules and the operational workflow. Mechanisms, measurements, and code
examples behind these rules live in `docs/embedded-reference.md`; durable
repository and release context lives in `docs/PROJECT_MEMORY.md`.

Project: Pocket Daily firmware, a fork of the open-source CrossPoint e-reader
firmware for Xteink X3/X4 (ESP32-C3). Mission: a lightweight, stable reading
experience on constrained hardware. We are building a dedicated e-reader, not
a Swiss Army knife: a feature that adds RAM pressure without significantly
improving reading is out of scope.

## Instruction files and shared memory

- `AGENTS.md` is read natively by Codex, OpenCode, the GitHub coding agent, and
  Claude Code (2.1.277 or later). Do not add `CLAUDE.md`, `.claude/CLAUDE.md`,
  or `CLAUDE.local.md` at the root or in any parent directory: Claude Code
  reads those *instead of* `AGENTS.md`. `*.local*` is gitignored, so a stray
  `CLAUDE.local.md` would silently drop this guide without showing in
  `git status`.
- Keep this file portable plain Markdown and well under 32 KiB, Codex's default
  limit for all `AGENTS.md` files combined. Refer to other documents by path;
  do not rely on `@path` imports, which only Claude Code expands. Before adding
  to this file, ask whether the rule is needed on every task; otherwise it
  belongs in the reference, a skill, or a scoped `AGENTS.md`.
- Directory-scoped rules live in nested `AGENTS.md` files. Claude Code loads
  them when it opens a file there, but Codex only loads those on the path to
  its working directory, so **read the scoped file before editing that
  subtree**:
  - `lib/Epub/AGENTS.md` — cache formats, version bytes, invalidation.
- Task procedures live in `.claude/skills/<name>/SKILL.md`. Claude Code
  discovers them automatically; every other agent reads the matching file
  directly when the task fits:

  | Skill | Read when you are… |
  |---|---|
  | `heap-discipline` | allocating memory: new/malloc/vector/string, buffers, caches |
  | `control-flow-clarity` | writing branching logic, state flags, modes, if/else ladders |
  | `hal-and-abstractions` | touching storage, input, display, settings, i18n, rendering |
  | `scope-discipline` | adding a feature, activity, lib, setting, or dependency |
  | `refactor-for-review` | refactoring, cleaning up, or preparing a change for PR |
  | `debug-crashes` | chasing a panic, reboot, hang, or watchdog timeout |
  | `firmware-deploy` | getting a build onto the device |
  | `fork-sync` | pulling upstream, opening an upstream PR |
  | `generated-content` | changing i18n YAML, HTML pages, or fonts |

- `docs/PROJECT_MEMORY.md` is the shared cross-session, cross-agent memory.
  Project facts, decisions, and hand-offs go there, never only into a
  tool-private memory (Claude auto memory, Codex or OpenCode state). A memory
  entry lands in the same change as the work it describes, so memory never
  gets ahead of the tree.
- Personal preferences belong in each tool's user-level file
  (`~/.claude/CLAUDE.md`, `~/.codex/AGENTS.md`,
  `~/.config/opencode/AGENTS.md`), not in the repository. Tool-local state
  (`.claude/` except `skills/`, `.codex/`) stays untracked.
- The sibling app repository `pocket-daily` follows the same contract from its
  own `AGENTS.md`; protocol changes must be verified on both sides regardless
  of which agent or repository starts the work.

## Read in this order

1. This file, in full.
2. The scoped `AGENTS.md` and skill for the subtree and task at hand.
3. Only the task-relevant source and tests; `docs/embedded-reference.md` when
   you need the mechanism or example behind a rule.
4. `docs/PROJECT_MEMORY.md` for repository boundaries and durable state.
5. `docs/release-checklist.md` before declaring a release complete.
6. For live-studio, live sync, or UI-pack work, `docs/live-studio-v1.md` and
   the companion design `docs/LIVE_STUDIO_DESIGN.md` in the sibling app
   repository.

## Agent reasoning rules

- Evidence: before proposing a change, cite the file path (and line numbers
  where useful) that justifies it.
- Do not assume a library or ESP-IDF function exists for the ESP32-C3 RISC-V
  target; check `open-x4-sdk` or official docs.
- Do not claim performance or memory gains without the technical mechanism
  (for example DRAM vs IRAM).
- Justify every new heap allocation (`new`, `malloc`, `std::vector`) or say why
  a stack/static alternative was rejected.
- After a fix, tell the user how to verify it (heap over serial, a specific
  cache file, a hardware step).

## Repository identity and boundaries

- Repository: `puritysb/pocket-daily-firmware`; default product branch `main`.
- Foundation remote: `upstream` → `crosspoint-reader/crosspoint-reader`.
- App repository: sibling `pocket-daily` directory next to this clone (on this
  host `/Users/puritysb/git/pocket-daily`). Absolute `~/github/` paths in older
  notes are stale; resolve the sibling relative to this checkout.

This repository owns reader behavior, device endpoints, on-device validation,
and flashing. The app repository owns iOS/iPadOS/macOS code and App Store
assets. Bluetooth records, endpoint payloads, file layouts, and update rules
are cross-repository contracts; verify both sides when any of them changes.

`main` carries the Pocket Daily product stack — `src/pocket_daily/` and
`src/activities/pocket_daily/` — layered on upstream. The AgentDeck daemon
provider (`src/agentdeck/`) was removed on 2026-09-25; the companion app is the
only content source. Being far ahead of `upstream/master` is expected.

- Pull upstream only with `./scripts/sync-upstream.sh`: merge, never rebase.
- An upstream PR contains zero product-stack files, so keep product and reader
  changes in separate commits from the start. Full procedure: `fork-sync`.

## Hardware and memory constraints

- ESP32-C3, single-core RISC-V at 160 MHz, ~380 KB usable RAM, **no PSRAM**,
  16 MB flash, SD card for books and caches.
- One framebuffer only (`EINK_DISPLAY_SINGLE_BUFFER_MODE`): X4 800×480 =
  48,000 B, X3 528×792 = 52,272 B. Grayscale rendering allocates a temporary
  buffer (`storeBwBuffer()`) that `restoreBwBuffer()` must free.
- Runtime heap and the largest contiguous block are release constraints. The
  Wi-Fi driver takes most of the ~97 KB free before the radio; raise the radio
  late, start optional services only when needed, and keep allocations held
  across a network operation small. Measured ledger in the reference.
- Do not raise the radio or log on a starved heap: Arduino's Wi-Fi event task
  and dev-build serial logging both allocate, and failure ends in `abort()`.
- Do not "fix" fragmentation with a permanent preallocated buffer; it lowers
  headroom for everything else without lowering the transient peak.
- `sta_recovery` alone wraps `esp_wifi_init` to bound driver buffer counts; do
  not promote it without hardware comparisons.

## Resource protocol

1. Locals under 256 bytes per function; larger buffers use `unique_ptr` or a
   static pool.
2. No repeated `new`/`delete` in loops; allocate once in `onEnter()` and reuse.
3. Constant data and lookup tables are `static constexpr` (class level) or
   `constexpr`, so they live in flash and compute at compile time.
4. No `std::string` or Arduino `String` in hot paths; `std::string_view` for
   reads, `snprintf` into fixed `char[]` for construction.
5. `std::vector`: `.reserve(N)` before any `push_back()` loop.
6. Never write a settings file on every interaction: guard with a value-change
   check and debounce reading-progress saves (exit or every N pages).
7. Bare `new` aborts on OOM under `-fno-exceptions`. Use
   `makeUniqueNoThrow<T>()` from `lib/Memory/Memory.h` (preferred) or
   `new (std::nothrow)`, null-check, and `LOG_ERR` before returning false. Raw
   `malloc` only when a C API takes ownership, with a comment saying so.
   Nothrow protects only code you control — framework allocations still abort,
   so keep headroom.
8. Prefer `std::unique_ptr`; avoid `std::shared_ptr`. Avoid `std::function` and
   unbounded template instantiation in library and render-loop code; use a
   function pointer plus context.

## Platform rules

- **HAL only.** Use `HalDisplay`, `HalGPIO`, and `HalStorage` (`Storage`), never
  the SDK classes. All SD access goes through `HalStorage`/`HalFile`; never
  touch SdFat, `SdSpiCard`, `FsBaseFile`, `SDCardManager`, or raw `FsFile`
  directly — SdFat is not thread-safe and concurrent use panics FreeRTOS.
- `DESTRUCTOR_CLOSES_FILE=1`: do not add `close()` for local files. Close
  explicitly only before removing or reopening the same path, and for member
  handles at their release point (`onExit()`).
- `std::string_view::data()` is not null-terminated; convert before any C API.
- ISR handlers are `IRAM_ATTR`; data they read is `DRAM_ATTR`. Never take a
  mutex from an ISR; use the `...FromISR` primitives.
- RISC-V faults on unaligned multi-byte loads: `memcpy` from byte buffers,
  never cast `uint8_t*` to a wider pointer (packed structs included).
- Errors: `LOG_ERR` then `return false` or fall back. No exceptions, no
  `abort()`; `assert(false)` only for impossible states, `ESP.restart()` only
  for recovery. Always log with `LOG_INF`/`LOG_DBG`/`LOG_ERR` from
  `Logging.h`, never raw `Serial`.
- C++20, no exceptions, no RTTI, `#pragma once`. Naming: PascalCase classes
  and files, camelCase methods/variables/members (no prefix), UPPER_SNAKE_CASE
  constants.

## Activities, tasks, and UI

- Activities are heap-allocated and deleted on exit. Whatever `onEnter()`
  allocates, `onExit()` frees in reverse order; delete FreeRTOS tasks with
  `vTaskDelete()` and close member files before destruction.
- Task stacks are in bytes: 2048 for simple rendering, 4096 for network or
  EPUB parsing; check `uxTaskGetStackHighWaterMark()` on crashes.
- Never assume 800 or 480: use `renderer.getScreenWidth()`/`getScreenHeight()`
  and `getOrientedViewableTRBL()`. Review all four orientations.
- Use `MappedInputManager::Button::*`, never raw `HalGPIO::BTN_*` (except in
  `ButtonRemapActivity`).
- Render through the `GUI` (UITheme) macro; do not hardcode fonts, colors, or
  positions.
- All user-facing text goes through `tr(STR_...)`; log text may be literal.
  Every font adds DRAM at runtime, not only flash.

## Generated files and caches

- Never hand-edit generated files: `src/network/html/*.generated.h` (from
  `data/html/`), `lib/I18n/I18nKeys.h`, `I18nStrings.h`, `I18nStrings.cpp`
  (from `lib/I18n/translations/*.yaml`). Edit the source, regenerate, commit
  the source only. Procedure: `generated-content`.
- Book caches live under `.crosspoint/` on the SD card. **Increment the format
  version before changing any binary layout.** The fork's
  `SECTION_FILE_VERSION` stays in the reserved 128–255 range; details and the
  next free number are in `lib/Epub/AGENTS.md`.
- `platformio.local.ini` holds per-machine settings and is never committed;
  never put serial ports or credentials in `platformio.ini`. Extend flags with
  `${base.build_flags}`.

## Native PlatformIO wrapper

Use `./scripts/pio.sh`, not a bare `pio` command. The wrapper selects the native
arm64 Homebrew installation on Apple Silicon and avoids loading an x86_64
PlatformIO interpreter with arm64 Python extensions. Set `PLATFORMIO_BIN` only
when an explicit executable override is required.

```sh
./scripts/pio.sh run -e default
./scripts/pio.sh check --fail-on-defect low --fail-on-defect medium --fail-on-defect high
```

Environments (`platformio.ini` is authoritative): `default` (development),
`gh_release` (production), `gh_release_rc`, `slim` (no serial), and the
opt-in `sta_recovery`, `network_diagnostics`
(`ENABLE_DEV_NETWORK_DIAGNOSTICS`; heavy diagnostics are not in the default
image), and `heapmap` audit builds. Every successful build stages an ignored `firmware/update.bin`. Read
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

- C/C++ or build changes: format, strict cppcheck, default firmware build (zero
  errors and warnings), and the complete host test suite.
- Protocol changes: add deterministic host tests and check the app-side
  implementation in `pocket-daily`.
- Generated content: edit its source, run the relevant generator, and follow
  `generated-content`.
- Release changes: validate `gh_release`, obtain X3 and X4 hardware sign-off,
  merge with green CI, then create the release tag.

Run formatting with `./bin/clang-format-fix`, then inspect the diff. Do not
silence cppcheck broadly. A narrow suppression must explain the embedded-system
reason and preserve meaningful defect detection. Fix CI failures before asking
for review.

Flag for the user what an agent cannot verify: behavior on hardware, all four
orientations, heap (`ESP.getFreeHeap()` above 50 KB, no leaks), and cache
re-parse after EPUB changes. Do not claim Bluetooth, Wi-Fi handoff, local
HTTP, SD-card, OTA, recovery, watchdog, or heap behavior from a host build.

## Hardware deployment

Use `firmware-deploy` for deployment. An agent may stage an artifact, but the
user installs it and confirms physical-device results. For iteration, prefer
the reader's File Transfer → Join a Network mode plus `scripts/pocket_put.py`
over SD-card swapping; the user has asked that hardware tests go through that
path. Serial monitoring: `python3 scripts/debugging_monitor.py`.

Developer flash loop (`ENABLE_DEV_REMOTE_FLASH`; default and experimental
sta_recovery builds, never gh_release):
after `./scripts/pio.sh run` and a `scripts/pocket_put.py` push of
`firmware/update.bin`, `curl -X POST http://<reader>/api/pocket/v1/dev/flash`
validates and flashes the staged image and reboots the reader into a one-shot
saved-network reconnect (failure falls back to the Wi-Fi chooser). The endpoint
and its boot marker are compiled out of `gh_release` builds.

## Git and release hygiene

- Inspect `git status`, `git branch --show-current`, and `git remote -v` before
  Git operations; never assume branch or remote names or push rights.
- Preserve unrelated changes. Before staging, cross-check against `.gitignore`
  and never commit ignored build output (`.pio/`, `build/`, `*.generated.h`,
  `compile_commands.json`, `platformio.local.ini`).
- Do not commit secrets, Wi-Fi credentials, passkeys, device identifiers, or
  private serial logs.
- Commit only when the user asks. Messages use `<type>: <summary>` with types
  `feat`, `fix`, `refactor`, `docs`, `test`, `chore`, `perf`. Branches use
  `feature/`, `fix/<issue>-`, `refactor/`, or `docs/` prefixes.
- CI must pass on `main` before tagging.
- A release tag publishes the production `gh_release` artifact. Never tag or
  publish before both hardware rows in `docs/release-checklist.md` are signed
  off with the tested commit and artifact hash.
