# Reader navigation across sleep and internal restart

Development fix, 2026-11-01. Release/distribution remains on hold. Both enrolled
X3 and X4 received the explicitly authorized developer candidate described below.

## Failure and correction

Pocket Daily opens an EPUB through `goToReaderFrom(PocketDaily, ...)`. With an
existing screen, ActivityManager queues the reader. On splashless wake there is
no current activity, so `replaceActivity()` immediately installs the reader and
calls `onEnter()`. The old origin assignment inspected only `pendingActivity`;
therefore the immediate path retained its new manager's default Home target.

`ReaderNavigation.cpp` now records the requested origin after successful reader
construction and before installation, equally for immediate and queued launches.
Failed construction preserves the active reader's origin. Ordinary opens inherit
the parent shell; internal reopens retain the book's existing origin.

Render-task label reads and loop-task destination updates use atomic enum values;
existing RenderLock handling still owns activity lifetime.

Sleep saves both the reader Back target (Home, Pocket Daily, Articles) and parent
shell (Home or Pocket Daily). Boot restores them before opening a screen. A book
restored from reader sleep keeps its saved origin; a wake shortcut opening a book
from a non-reader surface uses that surface's parent shell instead. Internal
reader-target restarts save and restore the same context. No per-page writes or
new framebuffer/large allocation are introduced; the context is two byte-sized
enums plus optional presence metadata in the existing state object.

The clean-sleep marker participates consistently in navigation and retained-frame
wake classification, including a power-cut wake identified as Other. GPIO
PowerButton alone with an ordinary boot screen does not restore an old context;
that cold boot uses the configured startup shell. Recovery/crash/dev routing and
the reader load-count guard retain their existing precedence.

## State compatibility

`/.crosspoint/state.json` gains an optional `readerNavigation` object:
`{"version":1,"reader":0,"shell":0}`. Reader values: Home=0, Pocket Daily=1,
Articles=2. Shell values: Home=0, Pocket Daily=1. Unknown versions, missing fields,
wrong types and out-of-range values are ignored. The old
`lastSleepInPocketDaily` boolean remains for backward-compatible fallback;
legacy data cannot reconstruct an Articles origin it never stored.

This preserves navigation/Back context, not arbitrary dialog contents, drafts,
menu selection positions or serialized activity callbacks.

## Verification

- Nine host cases exercise actual launch/state serialization code, including
  immediate/queued × three origins × two shells, repeated JSON round trips,
  legacy/malformed input, active Articles allocation failure, shell wake shortcuts
  clean-sleep/cold-boot classification and destination-label helpers.
- Reintroducing the old pending-only origin assignment in a separate source copy
  makes the immediate-launch regression fail; production source is unchanged.
- Complete host suite: 1,175 passed. Default build: passed with zero warnings.
- Private evidence: `build/sleep-navigation-20261101/` (logs, source hashes and
  mutation result). Deterministic tests simulate lifecycle decisions; they do not
  establish physical X3/X4 wake or e-paper/input timing.
- Both X3 and X4 were installed via the existing one-shot developer flash pipeline,
  then independently checked for exact identity and version:
  `1.0.0-dev-codex-preserve-sleep-navigation-07d79067-wa622e18d` (intermediate).
  A final review then added atomic destination access. Final candidate
  `1.0.0-dev-codex-preserve-sleep-navigation-07d79067-w7317c875` was installed on
  both devices and the owner repeated the same physical Sleep/Wake/Back trial
  successfully on both. Fresh exact-version state readbacks are retained separately
  as `manual-validation-final.json` and `raw-after-final-manual/`.
- The owner operated each device: Pocket Daily Select → book → physical Sleep →
  Wake → leftmost short Back → Pocket Daily → Same Wi-Fi. Both returned as
  expected. These are owner-operated physical trials, not automated button tests.
  Both subsequent state readbacks contain version1/reader1/shell1 and
  lastSleepFromReader=true. No new release acceptance is implied.
- Reading-menu wording was not physically checked: the owner did not open that
  menu during the X3 trial, and the simplified X4 trial did not ask for it.
  Footer/exit labels have code-review and host-test evidence, not a complete
  per-language/per-screen physical rendering sign-off.
- Hardware proof: `build/sleep-navigation-hardware-20261101/` contains the frozen
  BIN/ELF/MAP and identity (`artifact-v2/` is final), exact-source pin, fresh protected-state/diagnostic/staged
  firmware backups, per-device manual-validation/readback records and staging
  cleanup receipts. Original update.bin was present on X3 and absent on X4;
  cleanup restores that presence/bytes without flashing again. Settings are
  compared against backups; the owner's ordinary reading state is retained.
- X4 needed read-only app LAN discovery because its saved address was stale.
  The first Mac host failed to load its test bundle, and rebuilding the current
  app hit another session's uncommitted UI split. Those failures are retained.
  A detached, committed app `a8c7b6b` test host passed X4 discovery, with no
  Bluetooth calls; identity/version were rechecked before updating only its host
  hint. The other session's app sources were not changed.

## Contextual labels

- Library labels an actual shell exit Home only for the stock Home parent;
  Pocket Daily exits say Back. Search/collapsed-group/tab-focus back-outs keep
  Back. Degraded list mode uses the same exit condition as its action.
- Root File Browser uses the parent-shell label; firmware-picker cancellation
  says Cancel. Non-root directory navigation keeps Back.
- Articles Back returns to Pocket Daily and no longer advertises stock Home.
- Reader More and end-of-book exit rows identify Home, Pocket Daily or Articles
  according to the saved reader origin. The generic menu Back still cancels
  the menu and returns to reading.
- Pocket Daily's ambient/no-carousel footer uses fixed physical Home/Sync/
  Articles/Sync slots, matching its physical input handlers. It does not apply
  reader button remapping to those fixed actions; Confirm there routes to Sync.
