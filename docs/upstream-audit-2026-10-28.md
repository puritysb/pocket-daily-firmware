# Upstream synchronization audit — 2026-10-28

## Result and scope

The product contains the complete **stable upstream ancestry**, but does not
contain all subsequent development changes. No stable merge is pending.
Ancestry is not proof that every upstream behavior survived the fork's changes.
This audit inspected history, changed paths, merge conflicts and selected
high-risk source changes; it is not a behavioral review of all 177 commits.

The existing stable-only integration policy remains in force. No development
firmware changes were imported, no SDK checkout was moved, and no product
branch was merged, committed, pushed or released. The only executable change
is clearer read-only reporting in `scripts/sync-upstream.sh --check`.

## Pinned evidence

Fetched `origin` and `upstream`; also checked the upstream GitHub releases API.

| Input | Observed value |
| --- | --- |
| Product HEAD and `origin/main` | `3f0bade264278465594578721e68427e57fc527b` (0 ahead / 0 behind) |
| Historical 1.4.1 baseline | `2754a5ff01644d36cf0a17db98f28408666ba518` |
| Stable `upstream/master`, tag 1.6.5 | `93e98bb78702e29868a16a13b80c40e6b36ccdff` |
| Current common ancestor with develop | The same `93e98bb7` stable commit |
| Tag `2.0.0rc` | `91ed3ef8a48962e0066a22b149cab3dfa0a0ee05`; GitHub marks it prerelease |
| Development tip | `40a633e786de6b5351cbcd9fd16e8d337b2cf3aa` |
| Product SDK checkout | `111fdcc7f0176c3ee38391a160ee296bf492dbd8`, with local SDCardManager changes |
| Development SDK gitlink | `44427f8d51866848b26bea01197f499f9fee5c4b` |

The original 1.4.1 → 1.6.5 interval contains 370 commits and is already in the
product's ancestry. Stable → develop contains 177 commits: 117 through the RC
tag and 60 after it. These are ancestry counts, **not 177 missing fixes**:
some changes have downstream equivalents, some require adaptation, and some
add capabilities outside this product's current scope.

The public [release list](https://github.com/crosspoint-reader/crosspoint-reader/releases)
still lists 1.6.5 as the newest non-prerelease. Do not promote 2.0 RC merely
because `develop` is upstream's default contribution branch.

## Integration risk measured against source

- Stable → develop changes 588 paths; stable → product HEAD changes 673.
  Their intersection is 118 paths.
- Read-only `git merge-tree --write-tree --name-only HEAD <develop-sha>`
  reports **74 conflicted paths**. This is a diagnostic, not an integration.
- The observed worktree had 228 changed/untracked paths, including the SDK
  entry; 47 overlap upstream changes. The conflict count above excludes these
  uncommitted changes, so it understates the current integration workload.
- Reader, EPUB, fonts, HAL, network, settings and build inputs dominate the
  conflicts. Product-only modules do not eliminate the risk in shared code.

Do not reset, stash, overwrite or snapshot-commit the existing work merely to
make a merge command run. A future integration must preserve the actual local
candidate as well as committed HEAD. The ongoing source/hardware work is not
validated by this audit.

## Source-reviewed decisions and candidates

Line references describe the inspected worktree; use symbol names after edits.
“Candidate” means a source-level reason to investigate, not a cleared port or
a measured performance gain.

| Upstream change | Current product evidence | Decision / required proof |
| --- | --- | --- |
| `f1d0af1a` (#3838), SD-font kerning/ligature edge cases | `lib/EpdFont/SdCardFont.cpp:353,431,446` still uses 8-bit counts/loops through class 255. The fork has its own Cached/BoundedUI paths. | High-priority correctness candidate. Port the relevant bounds/loop/coverage fixes without replacing bounded font behavior. Test 255 classes, malformed class IDs, cached-glyph top-ups, ligature-only fonts, allocation failure and CJK layout; run ASan/UBSan and mutation checks. |
| `c1d99e56` (#4091), corrupt glyph records | Upstream adds packed-size and file-bound validation at `glyphEntryValid`; our `SdCardFont` implementation and fallback policy differ. | Review every metadata/bitmap entry path, including BoundedUI and glyph misses. Preserve fallback and I/O fault behavior; do not copy only the upstream UI notice. |
| `38280863` (#3830), recent-book allocation churn | `src/RecentBooksStore.cpp:85-105` still erases and reinserts an existing entry, but also calls product `sanitizeBook`. | Candidate for move/rotate and conditional metadata updates. Preserve sanitization, ordering, pruning and save behavior; compare allocation traces and largest free block before claiming a gain. |
| `5ab0f290` (#3847), power-mode race | `lib/hal/HalPowerManager.cpp:83-114` locks the BLE standby backend, but the legacy frequency-switch path reads the mode without holding the mutex. | Adapt only the legacy path. Verify lock acquisition vs frequency switch and every failure return; retain the separate PM-lock backend. Device frequency/radio tests remain necessary. |
| `149dd532` (#3733), very large spine index | `lib/Epub/Epub/BookMetadataCache.cpp:272-296` sizes targets to the entire spine. The fork separately stores spine sizes. | Candidate for bounded batches, not a whole-file replacement. Test nonuniform sizes across batch boundaries, absent entries and low-memory failure; preserve the fork's persisted formats. |
| `87f9c34c` (#3955), ZIP directory reads | Our `lib/ZipFile/ZipFile.cpp` carries ongoing inflate/retry work; directory scans and unaligned EOCD reads also need review. | Separate directory I/O work from decompression changes. Compare returned entries and instrument reads; cover malformed/truncated archives and the current inflate regressions. Do not infer that upstream's patch fixes all ZIP issues. |
| `70b3b1a3` (#4124), font-download allocation order | `src/activities/settings/FontDownloadActivity.cpp:230-233` refreshes the registry while the manifest DOM is live. | Candidate to refresh before TLS/DOM/arena allocations. Keep manifest integrity and registry failure behavior; measure heap and largest block on the C3. |
| `cdac66ff` (#3410), WebDAV one-byte GET | `src/network/WebDAVHandler.cpp:367-390` already explicitly streams and handles short writes. | Same defect already addressed downstream. Do not replace it merely to match upstream. The current 4 KiB local buffer is a separate stack-budget review item; compare short-write/disconnect behavior before consolidating on a HAL helper. |
| `e9245489` (#3685) plus `6c17b0ee` (#4150), separate FAT cache and write ordering | Product does not enable `USE_SEPARATE_FAT_CACHE` or carry upstream's `patch_sdfat.py`. | Do not import the optimization without its ordering fix. Review as a dependency pair with pinned library hashes and SD interruption/recovery testing; no claim of current exposure to the separate-cache defect. |
| `813aaa12` (#3834), BiDi “canonicalization” | Upstream `lib/MiniBidi/BidiUtils.cpp:166-173` at the pinned develop tip substitutes a specific Hebrew letter sequence with a shorter expression. The fork has no such substitution. | Exclude this behavior from an automatic port: it changes displayed content rather than merely its visual order. Any later BiDi optimization must preserve original text and have mixed RTL/LTR/diacritic fixtures. No intent is inferred from the patch. |
| `5d18bee7` (#4116), lazy BiDi workspace | Upstream moves static buffers to an allocation retained after first RTL use. | This exchanges always-resident memory for a potentially late contiguous allocation. Evaluate OOM/fallback and allocation timing; do not advertise unconditional heap savings. Exclude the preceding text substitution when adapting this file. |
| `13056043` (#4085), settings-list lifetime | Upstream's settings grouping and `.cpp` registry differ from our `src/SettingsList.h` and companion preference users. | Study the storage/lifetime approach; do not transplant the registry or its reported savings without app preference compatibility and measured allocation evidence. |

The remaining development commits are inventoried by history and changed
paths, not individually approved. In particular, plugins, page-turner BLE,
clippings, OPDS2, line-spacing redesign, extra device support and SDK updates
need separate scope and compatibility decisions. A fresh `fix:` title does
not establish applicability: #4227 concerns reader-menu paths also changed by
the upstream clipping feature and by local work.

## Product behavior that must survive a future port

| Boundary | Retain and verify |
| --- | --- |
| EPUB / progress | Raw DOM XPointer contract, visible offsets, bounded partial pagination, line-break gap window, and fork section version 136. Format/layout changes require cache invalidation and app-side golden checks. |
| Fonts / rendering | CJK fallback, Cached vs BoundedUI semantics, bounded metadata access, glyph-release policy and host rendering ABI. Pixel comparisons must cover X3/X4 and all four orientations. |
| Networking / release | Companion endpoints, bounded upload/download windows and receipts, verified TLS, lineage/base identity, production-only OTA rules and existing flash-slot limits. |
| BLE / sleep | Pocket radio ownership and admission budgets, existing sleep/wake and trial safeguards. Upstream page-turner ownership cannot independently claim the same radio. |
| SDK / hardware | Preserve HAL ownership and inspect the local SDCardManager patch before changing the gitlink. A clean build does not prove SD, radio, wake, battery or heap behavior. |
| Project instructions | Keep this fork's sole root `AGENTS.md` and product documentation; do not mechanically adopt upstream instruction-file layout. |

## Practices adopted from primary documentation

1. Fetch and inspect explicit refs before integrating. GitHub documents the
   upstream remote and fetch/merge workflow in
   [Syncing a fork](https://docs.github.com/en/pull-requests/how-tos/work-with-forks/syncing-a-fork).
   Here the stable source remains `master`; development is monitored separately.
2. Keep product history and use an explicit integration branch with the
   existing wrapper's `--no-ff --no-commit` merge. The
   [Git merge manual](https://git-scm.com/docs/git-merge) explains why
   `--no-commit` alone does not stop a fast-forward and warns about dirty-tree
   merges. Never resolve product conflicts with a blanket “ours” or “theirs”.
3. Track intent and dependencies, not just ancestry. Record each port's source
   commit, retained downstream adaptation, tests, and exclusions. Retire a
   duplicate only after checking equivalent behavior and failure paths.
4. Compare revisions of a focused patch series with
   [git range-diff](https://git-scm.com/docs/git-range-diff), alongside source,
   format, allocation and output comparisons. A similar diff does not prove
   runtime equivalence. Review reused conflict resolutions before staging.
5. Follow the current upstream
   [development workflow](https://github.com/crosspoint-reader/crosspoint-reader/blob/40a633e786de6b5351cbcd9fd16e8d337b2cf3aa/docs/contributing/development-workflow.md):
   focused changes, local checks and explicit scope. Its
   [downstream-foundation policy](https://github.com/crosspoint-reader/crosspoint-reader/blob/40a633e786de6b5351cbcd9fd16e8d337b2cf3aa/SCOPE.md)
   welcomes reusable primitives while leaving product semantics downstream.
   Upstream publication still requires approval of the exact content.

## Checks and limits

- `python3 scripts/test_sync_upstream.py`: **11/11 pass**. Tests cover the
  stable/development distinction, absent development branch, stable already
  current, dirty-tree preservation and continued refusal to merge prereleases.
- Mutation: changing development counting to use the stable ref makes both
  new ancestry assertions fail (2 expected failures, no test errors).
- `bash -n scripts/sync-upstream.sh`: pass. Live `--check`: stable 0,
  development 177. No merge state or local branch move.
- Existing seam checker: **fails with six unregistered inherited includes**
  in `PocketEndpoints.cpp`, `TrialLease.cpp` and `TrialReader.cpp`. The fixture
  tests pass, but `test_current_tree` fails. These are pre-existing local work,
  not caused by the reporting change; they remain an integration prerequisite.
- Firmware source and build inputs were not changed by this audit. Firmware
  builds, full C++ host suite, app contracts and device tests were not rerun.
  This is not release clearance or a no-regression claim for the local candidate.

Reproduce the pinned history/conflict inspection without touching the index:

```sh
./scripts/sync-upstream.sh --check
git rev-list --left-right --count HEAD...origin/main
git merge-base HEAD 40a633e786de6b5351cbcd9fd16e8d337b2cf3aa
git log --oneline 93e98bb7..40a633e7
git diff --name-only 93e98bb7 40a633e7
git merge-tree --write-tree --name-only 3f0bade2 40a633e7
python3 scripts/test_sync_upstream.py
python3 scripts/check_seam.py
```

The merge-tree command is expected to exit 1 for conflicts. Fetch updates
remote-tracking refs and downloads objects; it does not apply development
code to the worktree. Subsequent snapshots may have different counts.
