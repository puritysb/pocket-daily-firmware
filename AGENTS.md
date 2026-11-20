# Pocket Daily Firmware Agent Guide

This guide applies throughout this repository. When a task crosses into the sibling
checkout, read its root guide and any instructions scoped to the files you edit.

Project: Pocket Daily firmware, a fork of the open-source CrossPoint e-reader
firmware for Xteink X3/X4 (ESP32-C3). Mission: a lightweight, stable reading
experience on constrained hardware. We are building a dedicated e-reader, not
a Swiss Army knife: a feature that adds RAM pressure without significantly
improving reading is out of scope.

## Instruction files and shared memory

- Maintain one shared root guide in portable Markdown. Instruction discovery
  depends on the tool, its settings and the session's working directory.
  If this guide was not loaded, open it directly. Check for nested guides
  along the path to each file you edit. Keep repository instructions in
  `AGENTS.md` files rather than adding a second tool-specific root guide.
- The following subtree has its own guide:
  - `lib/Epub/AGENTS.md` — cache formats, version bytes, invalidation.
- The tracked procedures in `.claude/skills/<name>/SKILL.md` are canonical.
  Open the matching tracked file even if a local skill copy has the same name.

  | Skill | Read when you are… |
  |---|---|
  | `heap-discipline` | allocating memory: new/malloc/vector/string, buffers, caches |
  | `control-flow-clarity` | writing branching logic, state flags, modes, if/else ladders |
  | `hal-and-abstractions` | touching storage, input, display, settings, i18n, rendering |
  | `scope-discipline` | adding a feature, activity, lib, setting, or dependency |
  | `refactor-for-review` | refactoring, cleaning up, or preparing a change for PR |
  | `debug-crashes` | chasing a panic, reboot, hang, or watchdog timeout |
  | `firmware-deploy` | getting a build onto the device |
  | `fork-sync` | pulling upstream, opening or answering an upstream PR |
  | `generated-content` | changing i18n YAML, HTML pages, or fonts |

- `docs/PROJECT_MEMORY.md` is the shared cross-session, cross-agent memory.
  Project facts, decisions, and hand-offs go there, never only into a
  tool-private memory (Claude auto memory, Codex or OpenCode state). A memory
  entry lands in the same change as the work it describes, so memory never
  gets ahead of the tree. It holds current state only and stays under about
  300 lines: replace superseded entries and move history to `docs/history/`
  instead of appending session logs.
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
   `docs/history/` is a dated archive; read it only when tracing why something
   changed. Verify dated observations against current code or release records.
   Past authorization in memory does not extend the current task.
5. `docs/release-checklist.md` before declaring a release complete.
6. For live-studio or live sync work, `docs/live-studio-v1.md` and
   the companion design `docs/LIVE_STUDIO_DESIGN.md` in the sibling app
   repository.

## Agent reasoning rules

- Evidence: before proposing a change, cite the file path (and line numbers
  where useful) that justifies it.
- Do not assume a library or ESP-IDF function exists for the ESP32-C3 RISC-V
  target; check `freeink-sdk` or official docs.
- Do not claim performance or memory gains without the technical mechanism
  (for example DRAM vs IRAM).
- Justify every new heap allocation (`new`, `malloc`, `std::vector`) or say why
  a stack/static alternative was rejected.
- After a fix, tell the user how to verify it (heap over serial, a specific
  cache file, a hardware step).
- A test that guards new logic must fail when a fault is injected into that
  logic; a fixture with uniform values cannot guard indexing.
- Anything that reaches another repository (opening an upstream PR, pushing to
  its branch, commenting, editing its description) needs the owner's approval
  of the exact content. Read `fork-sync` first; state what was not verified on
  hardware and never write "verified by me" for the owner.

## Repository identity and boundaries

- Repository: `puritysb/pocket-daily-firmware`; default product branch `main`.
- Foundation remote: `upstream` → `crosspoint-reader/crosspoint-reader`.
- App repository: resolve `../pocket-daily` relative to this checkout.

This repository owns reader behavior, device endpoints, on-device validation,
and flashing. The app repository owns iOS/iPadOS/macOS code and App Store
assets. Bluetooth records, endpoint payloads, file layouts, and update rules
are cross-repository contracts; verify both sides when any of them changes.

`main` carries the Pocket Daily product stack — `src/pocket_daily/` and
`src/activities/pocket_daily/` — layered on upstream. The AgentDeck daemon
provider (`src/agentdeck/`) was removed on 2026-09-25; the companion app is the
only content source. Being far ahead of `upstream/master` is expected.

- Merge published CrossPoint release tags with `./scripts/sync-upstream.sh`.
  Follow the [fork-sync procedure](.claude/skills/fork-sync/SKILL.md) for
  selecting a release and tracking urgent backports.
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
- Run output under `build/<topic>-<date>/` holds evidence (logs, JSON, BINs,
  receipts), not extra toolchain or derived trees. Reuse `.pio/` and the shared
  host-test folders. For artifact maintenance, preview candidates with the
  sibling app's `scripts/prune_local_artifacts.py`. Review both checkouts'
  active work and backups before applying an authorized cleanup.
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
  check green CI and obtain explicit authorization before tagging.

For task-owned C/C++ files use `./bin/clang-format-fix --files <paths...>`
and inspect the diff. For documentation-only edits check links and run
`git diff --check`. Do not
silence cppcheck broadly. A narrow suppression must explain the embedded-system
reason and preserve meaningful defect detection. Fix CI failures before asking
for review.

Report which hardware behaviors, orientations and cache re-parses were tested.
For heap claims, compare free bytes and the largest contiguous block against
the phase-specific gates in `docs/nearby-sync-v1.md`. Do not claim Bluetooth, Wi-Fi handoff, local
HTTP, SD-card, OTA, recovery, watchdog, or heap behavior from a host build.

## Hardware deployment

Read [the developer pipeline](docs/developer-pipeline.md) and the
[deployment procedure](.claude/skills/firmware-deploy/SKILL.md). Use the
enrolled reader on Same Wi-Fi and pass the intended app checkout with
`--app`. Building, transferring, installing and verifying the running version
are separate steps. Use `--flash` only when the current task authorizes
developer installation on that reader. Production installation requires the
reader's physical confirmation. Report the tested image, device and limits.

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
