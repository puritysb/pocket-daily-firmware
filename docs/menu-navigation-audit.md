# Menu navigation audit (development, 2026-11-03)

Release, production preparation and distribution remain held. This audit concerns
both C3 X3/X4. Host results are not physical button, radio, power or sleep sign-off.
Evidence: `build/home-connect-20261102/`.

## Lessons learned

1. **Measure the transition boundaries before changing the algorithm.** Separate
   input detection, navigation request, reader destruction, destination entry,
   composition and panel refresh. The earlier X4 Back receipt7172ms contained
   6358ms in Home painting; optimizing reader exit alone did not fix that delay.
   The later physical receipt1910ms and owner1–2s report agree. Software-return
   Home times, optical completion and physical Back are different measurements.
2. **Keep physical release polling ahead of optional repaint work.** Select acts
   on release. A synchronous cover/AA repaint before dispatch made working input
   appear broken. Wait on the existing render lock, restore the framebuffer,
   dispatch release/navigation, then coalesce the destination repaint. Restoring
   framebuffer RAM does not reset a controller in terminal display deep sleep;
   resumable Home must retain a resumable panel lifecycle.
3. **Inspect storage/cache behavior behind apparently cheap text metrics.** Width
   queries use ink extents, so metrics-only warming was insufficient. Growing
   prefixes thrashed the eight-slot glyph overflow ring. Prepare a bounded UTF-8
   prefix in its actual font/style; preserve existing shaping and pinned page
   ownership. A larger permanent cache is not the default solution on no-PSRAM C3.
4. **Optimize the workload without replacing text semantics.** Eligible LTR
   wrapping now visits each glyph once; use the same ink/kerning/fixed-point
   rounding implementation as ordinary width measurement. Retain the old path
   for contextual shaping or whole-string font switching. Require equivalence
   with variable metrics, negative bearings, scaling, Unicode boundaries and
   complete framebuffers. Inject a fault to prove the regression detects it.
5. **Separate a setting value change from a list/resource change.** Refresh live
   values in place; rebuild only when row membership or resources/language change.
   Avoid routine SD dictionary/font scans and vector/callback reconstruction.
   Never retain a reference into a vector across its rebuild. Button hints must
   describe the actual current action; disabled/error views must also gate input.
6. **Attribute memory and performance to the measured phase.** Boot-wide minimum
   heap is not a Home-only low-water; free heap and largest block are distinct.
   Keep the existing activity-specific gates. Unchanged static RAM and functional
   samples do not establish battery life or leak freedom. The extra linear pass
   preserved roughly1.07s X4 Home timing versus prior1.08s; that sample does not
   establish an additional material wall-clock gain, despite better worst-case work.
7. **Freeze provenance before hardware iteration.** Pin the analyzer version
   (cppcheck2.11), build, then retain BIN/ELF/MAP/source hashes before another
   PlatformIO check/environment switch can remove outputs. Verify exact device
   identity and version, compare original state across installation, and restore
   only staging proven to belong to the run. Inspect failures before retrying;
   never replay an ambiguous flash/mode transition or stalled bulk download.

Owner feedback on2026-11-03 after the latest X3/X4 developer-install discussion:
“엄청나게 개선되었다.” Record this as strong overall subjective improvement.
The reply does not enumerate devices or menu cases, so it does not independently
close every row in the physical coverage matrix below. Keep the measured
before/after evidence and the scope of the owner's report separate.

## Changes and rationale

- Home wrapping previously measured each growing prefix, visiting up to
  n(n+1)/2 glyphs per line. `getSimpleTextFitBytes` now scans eligible LTR
  prefixes once with the same ink bounds, kerning and differential rounding
  used by `getTextWidth`. ASCII, precomposed Hangul and common Han qualify;
  whole-string font fallback, ligatures, RTL, combining marks and emoji retain
  the original path. This does not claim that all Unicode layout is linear.
- Existing bounded SD prefix preparation remains. No new persistent cache,
  bitmap, task or heap buffer is added. Newline-bounded preparation avoids
  loading text from the next explicit line. Ellipsis behavior is unchanged.
- Ordinary Settings edits update live values without rebuilding vectors or
  rescanning SD dictionaries/fonts. Short-power-button behavior changes row
  visibility and still rebuilds. Initial entry, font/text/language returns
  retain explicit rebuilds. Theme changes still reset UI tokens immediately.
- Capture the settings member pointer before rebuilding: the old code read
  `setting.valuePtr` after its vector entry had been destroyed.
- Settings footer distinguishes two-choice toggles from selection/actions.
  Articles labels its cleanup action Delete; empty/unavailable entries do not
  advertise Open. No action or parent destination changes with these labels.

## Flow coverage

| Flow | Source reviewed | Automated evidence | Physical status |
| --- | --- | --- | --- |
| Pocket Daily → book → Back | Home input, ReaderNavigation, ActivityManager | Ordered origin/exit tests; render timing | Previous X4 owner pass; new candidate pending |
| Book → Sleep/Wake → Back | main sleep state, ReaderNavigation | Origin/shell persistence, invalid state, cold-boot distinction | Earlier X3/X4 owner pass; new candidate pending |
| Pocket Daily → Articles → book → list → Back | ArticlesActivity, leaveReader | Articles origin/footer tests | Pending |
| Home/Pocket Daily → Library → Back | LibraryListActivity::loop, goToShell | Origin/shell tests, bounded font lifecycle | Pending |
| Browser folder/file options → Back | FileBrowserActivity, OptionPopup, UiListActivity | Shared font/layout regressions | Pending |
| Settings tab → row → popup/subscreen → Back | SettingsActivity, OptionPopup, UiTabListActivity | Full firmware build/analysis; shared rendering tests | Pending |
| Sync/Wi-Fi mode picker → cancel/Back | NetworkModeSelectionActivity, WifiSelectionActivity, CrossPointWebServerActivity | Prior connection receipts; no new network-switch claim | Pending |
| Card/empty/error → Back | HomeInputs/ContentPresentation/PresentationSlot | Ready-view gating, Back consumption, redraw/failed reload tests | Pending |

These tests exercise actual shared navigation/renderer units, not a complete
hardware activity-stack simulation. Wi-Fi teardown deliberately retains its
restart boundaries; USB raw-SD ownership and firmware confirmation are excluded
from destructive testing. Do not remove their waits as a latency optimization.
Saved reader origin/shell is not proof of restoring every arbitrary submenu or
an unfinished dialog across power-off; exact submenu restoration remains a
separate unverified behavior in this audit.

## Verification so far

- 1,188 host tests passed, including prefix equivalence with nonuniform glyph
  metrics, negative bearings, kerning and 0.5/1/1.5 scaling. Visits are bounded
  to one per examined glyph. An injected off-by-one fit fails; restored code passes.
- Legacy-loop oracle: both X3/X4 panel sizes, all four orientations, narrow and
  wide widths, 1/2/4 lines, word boundaries, CR/LF and the 191-byte UTF-8 boundary
  give identical line counts and framebuffer bytes. Panel guards remain intact.
- Development default/BLE standby builds and strict analysis are recorded in
  `menu-*-build.log` / `menu-*-cppcheck.log`. No local production build was run.
- X3 interim w0216a162 installed with an exact-version receipt. Automatic Home
  composition522ms + refresh484ms =1006ms; boot minimum81076B. Preferences,
  profile, reading and content state unchanged. No before/after X3 speedup or
  physical Back claim follows from this one automatic software-return trial.
- Original X3 staged image freshly backed up (6489664B); X4 original staging
  absence verified before installation. Final restoration receipts are recorded below.

## Latest hardware checkpoint: wdd553a3b

- Frozen `artifact-menu-guard`:6520048B, SHA256
  `3941384eb47bdece1c81b990df13a9d13fc8d9360a0d549019dadf0b43018ab0`.
  Matching BIN/ELF/MAP and source-pin retained. This adds explicit Back-only
  input on Articles errors, matching its disabled action/direction hints.
- Exact-version installations: X3 run `20261102T164414-ba1dbb`, X4 run
  `20261102T164720-3105e5` under `build/dev-pipeline-runs/`.
- Automatic Home: X3 compose521 + refresh485=1006ms, boot-min81076B;
  X4 compose438 + refresh633=1071ms, boot-min84176B. Those boot minima are not
  activity-local low-water marks. Software trials do not establish physical Back.
- Strict pinned cppcheck2.11 passed both environments after the error guard;
  warning-free development builds and host1188 passed. No local production build.
- Static RAM data22689B/bss49608B unchanged versus wd1bc04f2; code+608B.
  `menu-guard-memory-sections.json` records the actual ELF sections.
- X3 original6489664B staged image restored with owned publication/CRC checks;
  X4 original absence restored. Pre-install versus final preferences/profile/
  reading/content snapshots match on both. Restoration and state receipts are
  under `x3-wrap-final/` and `x4-menu-final/`.
- Both readers were left on Home for owner-operated book/Back/Sleep-Wake,
  Library/Files, Settings/picker and Articles tests. The owner subsequently
  reported strong overall improvement (see Lessons learned); individual route
  results remain unspecified. No full-menu, leak or battery sign-off is inferred.

## Final review and PR update (2026-11-03)

- Reviewed code commits: `4bab9a23` (shared ink measurement), `d6467075`
  (single-pass prefix fit), `bf5c3e20` (Settings/menu behavior). All32 source
  pins from installed wdd553a3b match the reviewed final tree; no new device
  installation or production preparation was performed for this review.
- Refactor-only host1186 and final host1188 passed. Final logs:
  `final-review-refactor.log`, `final-review-host-build.log`,
  `final-review-host-tests.log`. Seam boundary checks and diff checks passed;
  the existing SDK storage patch is excluded from the commits.
- The final PR head must be checked independently of the older fcee20e1 CI run.
  Keep PR28 draft and do not infer per-menu hardware completion from overall
  positive feedback. Merge, release, TestFlight and distribution remain held.
