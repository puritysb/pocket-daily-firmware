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
./scripts/sync-upstream.sh --check   # fetch and report only
./scripts/sync-upstream.sh --dry-run --ref <stable-commit>
# In a clean, explicitly selected integration branch:
./scripts/sync-upstream.sh --branch <existing-branch> --ref <stable-commit>
# Resolve, verify default + gh_release + host + strict cppcheck + app contracts.
# The script never switches branches, commits, rebases, or pushes.
```

Upstream's stable line is **`master`**, not `main`; releases land there. Upstream also has
`develop` (its default branch, ahead of `master`) — pull that only when you explicitly want
pre-release work. Upstream PRs, however, target `develop` (below).

Our `main` being far ahead of `upstream/master` is **expected, not drift**.

Why merge: `main` has Pocket Daily product commits on top. A rebase replays all of them and forces
re-resolving the same conflicts every sync; a merge resolves once and preserves the history.
Conflicts land almost exclusively on the **Pocket-touched shared files** (`ActivityManager`,
`HomeActivity`, `WifiSelectionActivity`, theme files; see `docs/SEAM.md`) — **resolve by keeping
BOTH sides**. `src/pocket_daily/*` files never conflict.

## Contributing a general feature upstream

An upstream PR is public, carries the owner's name, and costs maintainers and
reviewers real time. Every step that reaches `crosspoint-reader/crosspoint-reader`
(opening, pushing to the PR branch, commenting, editing the description, changing
draft state) needs the owner's approval of the exact content first. Preparing
branches, tests and drafts locally does not.

The PR must contain **zero Pocket Daily product files**. That is only cheap if you plan for it:
keep reader/rendering changes and product changes in **separate commits from the start**.

### Before opening

1. **Read upstream's own rules** on the target branch: `docs/contributing/development-workflow.md`,
   `AGENTS.md`, `SCOPE.md`, `.github/PULL_REQUEST_TEMPLATE.md`. They change; do not rely on this file.
2. **Search for duplicates**, open and closed: issues and PRs touching the same idea or files
   (`gh search prs/issues --repo …`, `gh pr list --json files`). Cite any overlap in the PR and say
   why this one differs (crosspoint-reader#1528 was the same optimisation, held back for heap reasons).
3. **Branch from `upstream/develop`** (upstream's default branch and PR target; `master` is the stable
   line this fork merges). Use a separate worktree so product files cannot leak in:
   ```bash
   git fetch upstream develop:refs/remotes/upstream/develop
   git worktree add ../crosspoint-upstream-pr upstream/develop
   cd ../crosspoint-upstream-pr && git submodule update --init freeink-sdk
   git switch -c upstream-pr/<topic>
   ```
   Re-port the change onto upstream's current code; do not assume the fork's version applies.
4. Touching a cache format? On the **upstream branch** bump the version to
   **upstream's current value + 1** (e.g. `SECTION_FILE_VERSION` in `lib/Epub/Epub/Section.cpp`).
   On **product `main`** the same constant stays in the reserved 128–255 range — the two
   branches deliberately number differently. See `lib/Epub/AGENTS.md`.
5. **Meet the evidence standard** in `docs/fork-delta-register.md` (defect, mechanism, numbers,
   no-regression proof, tests, extraction, heap history, disclosure). In particular:
   - A test must fail before the change and pass after it, **and fail when a fault is injected into
     the new logic** (off-by-one, wrong index). Check that the fixture can tell the cases apart: a
     stub with uniform values proves nothing about indexing.
   - Compare behaviour with the base differentially on varied input, not only on a few fixed cases.
   - Any new or larger allocation needs an allocation trace equal to the base or a device measurement
     of the largest free block. A stack alternative needs the compiler-reported frame
     (`PLATFORMIO_BUILD_FLAGS=-fstack-usage`) of the function and every helper it adds.
   - Re-read every number in the description against the source (`FOOTNOTE_HREF_LEN` is 256, not 128).
   - Force a rebuild between A/B runs (`touch` the file after a pause): a checkout within the same
     second as the last build can leave a stale binary and a false result.
6. **Run upstream's local checks** in the worktree: `./bin/clang-format-fix`, the full host suite,
   `pio check --fail-on-defect low --fail-on-defect medium --fail-on-defect high`, `pio run`.
7. Push to `origin` as `upstream-pr/<topic>` and open a **draft** PR against `develop` with the
   template filled in:
   ```bash
   gh pr create --repo crosspoint-reader/crosspoint-reader \
     --base develop --head <your-user>:upstream-pr/<topic> --draft
   ```
   Mark it ready only when the description's "not verified" list is empty or the maintainers ask.

### What the description must say

- What was verified on a host, and **what was not verified on hardware** (heap, largest free block,
  stack high-water, timing, orientations). Upstream's own checklist calls these the human tester's scope.
- Memory and stack cost as measured, including costs that look small. Do not argue safety from
  object lifetimes; a transient vector moved later allocations and cost 13 KB of contiguous heap.
- The AI-usage line as it happened: which tool wrote it and who ran which checks. Never
  "verified by me" for checks the account owner did not perform.
- Numbers with their source (host stub, device, which commit). Label downstream (fork) measurements
  as such.

### Responding to review

- Reproduce the reviewer's finding before replying (the host allocation trace matched the reported
  13,312 B exactly). If they are right, say so plainly and name what was wrong on our side.
- Fix, re-verify with the checks above, and only then post. Add a commit rather than force-pushing,
  so reviewers can see what changed; correct the description as well as commenting.
- Put the PR back to draft while a known defect or an unverified revision is open.
- State what the new revision trades away (the 32-entry window gives up part of the gain on dense
  lines) and what is still unverified. Do not promise device measurements the owner has not agreed to.
- Apply the same fix to fork `main` if the change is carried there, and record the review in
  `docs/fork-delta-register.md` and `docs/PROJECT_MEMORY.md`.

## Self-review

- Merged, not rebased? Do `src/pocket_daily/*` and the hooks in `docs/SEAM.md` still build?
- Conflict resolutions in shared files: did you keep **both** sides, or silently drop the
  Pocket Daily half?
- Upstream PR: `git diff --stat upstream/develop...HEAD` — does any `src/pocket_daily/` path appear?
  If yes, the branch is not ready.
- Upstream PR: owner approved the exact text? Duplicates searched? Tests fail under an injected fault?
  Heap trace or frame size measured? "Not verified on hardware" stated? Opened as a draft?
- Cache version bumped on the correct numbering line for the branch you are on?
- `./scripts/pio.sh run` clean before pushing.
