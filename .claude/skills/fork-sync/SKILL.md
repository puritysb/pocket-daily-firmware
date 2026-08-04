---
name: fork-sync
description: Keeping this fork in sync along both axes. Use when pulling upstream crosspoint-reader changes, resolving sync conflicts, contributing a general feature back upstream, opening an upstream PR, or re-porting the AgentDeck ESP32 wire contract into src/agentdeck. Covers merge-not-rebase, splitting AgentDeck code out of upstream PRs, cache-version numbering per branch, and the downstream protocol port.
---

# Fork Sync

This repo syncs in **two directions**, and confusing them is the main failure mode.

- **Upstream axis** — `crosspoint-reader/crosspoint-reader` → here. Reader/EPUB/rendering
  improvements flow in; general fixes flow back out as PRs.
- **Downstream axis** — the **AgentDeck** repo's ESP32 wire contract → `src/agentdeck/*`.
  A hand-port, not a dependency. Nothing flows back this way.

`origin` = your fork (`puritysb/crosspoint-reader`), `upstream` = the parent project. Local
`master` intentionally carries the private **"AgentDeck Decision Card"** stack
(`src/agentdeck/`, `AgentDashboardActivity`, agentdeck icons — ~2,700 lines) on top of
upstream. **The AgentDeck code is fork-only and never goes upstream.**

## Pulling upstream — merge, never rebase

```bash
./scripts/sync-upstream.sh           # fetch + report + merge upstream/master into master
./scripts/sync-upstream.sh --check   # report pending upstream commits only, don't merge
# then: pio run  (verify build) && git push origin master
```

Upstream's stable line is **`master`**, not `main`; releases land there. Upstream also has
`develop` (a few commits ahead) — pull that only when you explicitly want pre-release work.

Our `master` being far ahead of `upstream/master` is **expected, not drift**.

Why merge: `master` has ~20 AgentDeck commits on top. A rebase replays all of them and forces
re-resolving the same conflicts every sync; a merge resolves once and preserves the history.
Conflicts land almost exclusively on the **AgentDeck-touched shared files** (`ActivityManager`,
`HomeActivity`, `WifiSelectionActivity`, theme files) — **resolve by keeping BOTH sides**. New
`src/agentdeck/*` files never conflict.

## Contributing a general feature upstream

The PR must contain **zero AgentDeck files**. That is only cheap if you plan for it:

1. Keep reader/rendering changes and AgentDeck changes in **separate commits from the start**.
   Mixing concerns in one commit makes extraction painful later.
2. Branch from upstream and cherry-pick only the relevant commits:
   ```bash
   git checkout -b feature/<name>-upstream upstream/master
   git cherry-pick <epub-commit> [<font-commit> ...]   # exclude AgentDeck commits
   ```
3. Touching a cache format? On the **upstream branch** bump the version to
   **upstream's current value + 1** (e.g. `SECTION_FILE_VERSION` in `lib/Epub/Epub/Section.cpp`).
   On **fork `master`** the same constant stays in the reserved 128–255 range — the two
   branches deliberately number differently. See `lib/Epub/CLAUDE.md`.
4. `pio run`, push to `origin`, open a **draft** PR (public third-party repo — discuss with
   maintainers before marking ready):
   ```bash
   gh pr create --repo crosspoint-reader/crosspoint-reader \
     --base master --head <your-user>:feature/<name>-upstream --draft
   ```

## Downstream: AgentDeck wire-contract port

`src/agentdeck/*` is a **hand-port** of AgentDeck's ESP32 wire contract — the files carry a
*"TRIMMED port of AgentDeck esp32/src/net/protocol"* header. The SSOT lives in the AgentDeck repo:

- Human-readable client subset: `AgentDeck/docs/esp32-client-contract.md`
- Machine-readable: `AgentDeck/shared/src/protocol.ts` (`DISPLAY_FORWARDED_EVENTS` /
  `SERIAL_FORWARDED_EVENTS`) and the `sendDeviceInfo` field list in
  `AgentDeck/esp32/src/net/protocol.cpp`

There is **no C/C++ codegen** for this contract (quicktype emits Swift/Kotlin only — unusable
on the no-PSRAM C3 with ArduinoJson). **Drift is a discipline, not a build gate.**

When AgentDeck bumps the contract (adds/renames a forwarded event, changes a `device_info`
field), re-port the affected files:
`src/agentdeck/{ws_client,protocol,mdns_discovery,udp_discovery,agent_state,agent_commands}.*`.
Keep the port minimal — a display-only client may accept-and-ignore any inbound `type` it
doesn't render.

The fork emits both `client_register` and `device_info` on connect, with runtime board strings
`xteink_x3` / `xteink_x4`; `display_state` / `set_orientation` may remain accept-and-ignore for
the reader activity. The AgentDeck side of this discipline lives in
`AgentDeck/docs/esp32.md § Downstream client port sync`.

**E-ink dashboard layout** is the one part with tooling: the allocation-free geometry SSOT is
`esp32/src/ui/eink/eink_dashboard_layout.h` in AgentDeck, mirrored byte-for-byte here by
AgentDeck's `scripts/sync-xteink-eink-dashboard.sh`. Run it with `--check` after any layout
change. CrossPoint keeps its own GfxRenderer, font loading, button hints, and detail
interaction — only header/card/usage/activity/control geometry and status classification are
shared.

The behavioural contract for that dashboard (which options become buttons) is a correctness
invariant, not a sync step — it lives in `src/agentdeck/CLAUDE.md`.

## Self-review

- Merged, not rebased? Is `src/agentdeck/*` still present and building after the merge?
- Conflict resolutions in shared files: did you keep **both** sides, or silently drop the
  AgentDeck half?
- Upstream PR: `git diff --stat upstream/master...HEAD` — does any `src/agentdeck/` path appear?
  If yes, the branch is not ready.
- Cache version bumped on the correct numbering line for the branch you are on?
- `pio run` clean before pushing.
