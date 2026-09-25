---
name: fork-sync
description: Keeping this fork in sync with upstream. Use when pulling upstream crosspoint-reader changes, resolving sync conflicts, contributing a general feature back upstream, or opening an upstream PR. Covers merge-not-rebase, splitting Pocket Daily product code out of upstream PRs, and cache-version numbering per branch.
---

# Fork Sync

This repo syncs with **upstream** — `crosspoint-reader/crosspoint-reader` → here.
Reader/EPUB/rendering improvements flow in; general fixes flow back out as PRs. (The former
downstream AgentDeck wire-contract port, `src/agentdeck/*`, was removed on 2026-09-25.)

`origin` = the Pocket Daily firmware product (`puritysb/pocket-daily-firmware`),
`upstream` = the parent project. Local `main` intentionally carries the Pocket Daily product
stack on top of upstream. **Pocket
Daily product code is downstream-only and never goes upstream.**

## Pulling upstream — merge, never rebase

```bash
./scripts/sync-upstream.sh           # fetch + report + merge upstream/master into main
./scripts/sync-upstream.sh --check   # report pending upstream commits only, don't merge
# then: ./scripts/pio.sh run  (verify build) && git push origin main
```

Upstream's stable line is **`master`**, not `main`; releases land there. Upstream also has
`develop` (a few commits ahead) — pull that only when you explicitly want pre-release work.

Our `main` being far ahead of `upstream/master` is **expected, not drift**.

Why merge: `main` has Pocket Daily product commits on top. A rebase replays all of them and forces
re-resolving the same conflicts every sync; a merge resolves once and preserves the history.
Conflicts land almost exclusively on the **Pocket-touched shared files** (`ActivityManager`,
`HomeActivity`, `WifiSelectionActivity`, theme files; see `docs/SEAM.md`) — **resolve by keeping
BOTH sides**. `src/pocket_daily/*` files never conflict.

## Contributing a general feature upstream

The PR must contain **zero Pocket Daily product files**. That is only cheap if you plan for it:

1. Keep reader/rendering changes and product changes in **separate commits from the start**.
   Mixing concerns in one commit makes extraction painful later.
2. Branch from upstream and cherry-pick only the relevant commits:
   ```bash
   git checkout -b feature/<name>-upstream upstream/master
   git cherry-pick <epub-commit> [<font-commit> ...]   # exclude product commits
   ```
3. Touching a cache format? On the **upstream branch** bump the version to
   **upstream's current value + 1** (e.g. `SECTION_FILE_VERSION` in `lib/Epub/Epub/Section.cpp`).
   On **product `main`** the same constant stays in the reserved 128–255 range — the two
   branches deliberately number differently. See `lib/Epub/AGENTS.md`.
4. `./scripts/pio.sh run`, push to `origin`, open a **draft** PR (public third-party repo — discuss with
   maintainers before marking ready):
   ```bash
   gh pr create --repo crosspoint-reader/crosspoint-reader \
     --base master --head <your-user>:feature/<name>-upstream --draft
   ```

## Self-review

- Merged, not rebased? Do `src/pocket_daily/*` and the hooks in `docs/SEAM.md` still build?
- Conflict resolutions in shared files: did you keep **both** sides, or silently drop the
  Pocket Daily half?
- Upstream PR: `git diff --stat upstream/master...HEAD` — does any `src/pocket_daily/` path appear?
  If yes, the branch is not ready.
- Cache version bumped on the correct numbering line for the branch you are on?
- `./scripts/pio.sh run` clean before pushing.
