# Project memory entries superseded by the 2.0.0 base (2026-11-06)

Archived from `docs/PROJECT_MEMORY.md` → "Upstream base and fork delta".

- Audit 2026-10-28: `main`/`origin/main` `3f0bade2` includes stable `93e98bb7`;
  develop `40a633e7` adds 177 ancestry commits, 74 merge-conflict paths. No RC
  firmware imported; `--check` reports development separately. Candidates,
  exclusions and existing seam failures: `docs/upstream-audit-2026-10-28.md`.
- Reader A/B loop: `docs/reader-benchmark.md`; raster (0.9–6.0%) and font-I/O
  (107→57 reads, no speedup), and idle-font reuse (2.7% English/1.0% Korean-image,
  heap regression) pilots rejected; source reverted. Evidence/recovery linked there.
- Base: CrossPoint 1.6.5 (`93e98bb7`) integrated on `main` at `ee188fe7`
  (PR #4), FreeInk SDK `111fdcc7`. wolfSSL intentionally excluded.
  `docs/upstream-integration-1.6.5.md`.

## Current 1.0.0 preparation — 2026-11-01

- Candidate source `6025da03`, gh_release 1.0.0, 6,374,848B, SHA256
  `4376a1f99da9d8d4e6369a882a7038436c6889a4ef5857d76d027e6e4ed84d13`.
  Exact-image status and remaining gates: `docs/release-1.0.0-candidate.md`.
- Home uses measured title/author height to maximize covers in both layouts.
  Host renderer re-imported by the app; developer screen evidence covers eight
  connected previews plus actual Home and sleep, all ten pixel comparisons exact.
  `build/dev-pipeline-runs/20261031T201436-2a15e9/report.json`.
- Host1156/Python310/strict cppcheck/default and gh_release builds passed.
  Candidate artifacts and current device receipts: `build/release-prep-20261101/`.
  Latest installed versions and restoration status live in the app hand-off.
- Actual X3 slots0x10000/0x780000 each0x770000; X4 slots0x10000/0x650000 each0x640000.
  Preserve each device's own whole return slot before a bounded production trial.
- Historical memory/reading and BLE/settings baselines are archived in
  `docs/history/PROJECT_MEMORY-candidates-before-1.0.0-prep-2026-11-01.md`.
  Prior image results keep their original scope; release requires current-image gates.

## Sleep navigation fix in development (2026-11-01)

- Immediate reader installation after wake bypassed the pending-only origin assignment, so Back went Home. Origin is now set before installation; reader origin and parent shell persist through sleep/internal reader restart, with legacy fallback.
- Branch `codex/preserve-sleep-navigation`; final developer w7317c875 installed on X3/X4. Owner-operated Sleep/Wake/Back returned to Pocket Daily on both; state readbacks confirm context. Host 1,175/default build/strict analysis pass. Contextual footer/menu labels corrected with atomic destination reads/writes; full physical label coverage is unverified. Proof: `docs/sleep-navigation.md`, `build/sleep-navigation-hardware-20261101/`. Release/distribution held.

## Keyboard remote development (2026-11-01)

- Owner: default OFF; release/distribution stays on hold. Contract: `docs/keyboard-remote-v1.md`.
- Merged app #19 / firmware #26 into main (2026-11-01); feature branches/worktrees cleaned. Other app work and existing SDK storage patch preserved.
- X3 opt4 `1.0.0-dev-codex-keyboard-remote-a3fd9694-w319d57e0`: 24 turns passed; median ACK 1.601s (was 4.789s), 22 streamed / 2 chapter fallbacks; checkpoint min free/block 56,824/40,948B.
- X4 lacked a reader bond; owner ended further pairing trials and accepted code-level verification only. No X4 remote/power hardware sign-off.
- 4KiB text streaming retains BLE; chapter/image fallback retains radio-off rendering and unchanged memory gates. 80MHz idle/panel-off policies implemented; USB-off follow-up observed 47mA connected idle vs prior 49mA, awake OFF 23–24mA (short onboard comparison). Evidence/backups: `build/keyboard-remote-perf-20261101/`; battery follow-up `build/keyboard-remote-power-20261101/`; baseline `build/keyboard-remote-20261101/`.
