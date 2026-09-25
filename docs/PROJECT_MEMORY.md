# Pocket Daily Firmware Project Memory

This is concise, repository-owned context for future sessions. It is not a chat
transcript. Current source and release records override dated observations.

## Repository split

- 2026-09-25 AgentDeck daemon removed; app-provided glance (docs/
  pocket-glance-v1.md). Deleted src/agentdeck/**, the Pocket Daily daemon path
  (Wi-Fi join, discovery, pairing, feed/outbox, pull + WS OTA, asset sync,
  attention/decision UI, deck cache, timed-wake cadence in main.cpp and
  HalPowerManager/HalGPIO), the two AgentDeck settings (keyed JSON: old files
  load), provider glyphs and agentdeck-surface.json. New POST
  /api/pocket/v1/glance (Sync profiles, content admission, strict ≤2048 B JSON,
  404 B CRC record /.crosspoint/pocket-glance.bin saved temp→verify→swap with
  .bak fallback); status `pocketGlance: 1`; an unset clock is set from
  savedEpoch; utcOffsetMinutes drives the daily-word day and the local-day roll.
  Profile provider/monitor still parse but are not advertised or shown;
  defaults are reading, study. Static RAM 129,944 → 113,272 B (default), flash
  6,111,695 → 5,980,237 B. 389 host tests. Not yet exercised on a reader.

- 2026-09-25 My cards (docs/pocket-profile-v1.md): profile IDs `word` (Home
  item 5, the daily word as its own page; Study then shows only app cards) and
  `card` (sleep section 5, first app card with its image, shown even while a
  book is open). App-card Home pages and the pinned sleep card draw the card
  image via Env::drawCardImage (device: drawAppCardImage from the active
  revision; host: pdui_set_cards). Images shrink to fit, never enlarge
  (fitContentImage/contentImageHeight). Brief sections that cannot fit are
  skipped instead of overlapping the status line. Strings STR_POCKET_MY_CARDS
  and STR_POCKET_DAILY_WORD (en, ko). 406 host tests; static RAM unchanged.

- 2026-09-25 content read (docs/content-read-v1.md): GET
  /api/pocket/v1/content/file serves 4 KiB chunks of a published revision's
  manifest/card/image leaves read-only (content admission, no active stream
  transfer, publishedFilePath confinement); status `contentRead: 1`. The app
  verifies every hash and reviews before replacing its draft. Host tests 401;
  not exercised on a reader.

- 2026-09-25 Home rows: PocketDaily::Home::homeRowSources resolves the profile
  to row sources (order, app cards else daily word, items without content
  skipped) for both collectOverview and the host preview; HomeRowSources host
  test and the SyncRouteBoundaries source check cover it. 400 host tests.

- 2026-09-25 P1-3 (reader battery empty; host only): Home and Daily Brief
  drawing moved out of PocketDailyActivity into src/pocket_daily/home
  (HomeRenderer: renderHome/renderBrief with Env callbacks for header, cover and
  font resolution; HomeDrawing: glyphs, poster, strips). The activity builds
  views and calls them. PocketProfileJson.cpp split from PocketProfile.cpp so
  the host links the model without ArduinoJson. Host ABI adds
  pdui_render_home/pdui_render_brief (ABI 1) via host/HostHome.cpp. 399 host
  tests pass; default and gh_release build with no warnings, static RAM
  unchanged at 129,944 B; local cppcheck 2.20 finds nothing new. Not yet
  flashed or compared with a device capture.

- 2026-09-25 preferences POST is all-or-nothing: PreferencesUpdate validates
  the whole body (JSON integers only, ranges from CrossPointSettings, cover
  accepts bool or int, unknown keys ignored) before any SETTINGS change, and a
  failed save restores the previous values. Fixes the P0 finding where a valid
  startupApp stayed live after a later field returned 400. 4 host tests; local
  cppcheck 2.20 found nothing new. The PlatformIO strict check stays blocked:
  the espressif32 platform reinstalls tool-cppcheck 2.20 while `pio check`
  wants ~1.21100 and its mirror download loops on DNS failures.

- 2026-09-25 P1-2 (docs/pocket-profile-v1.md): Pocket Daily profile model,
  strict ArduinoJson parser, 32-byte two-slot record store loaded at boot in
  ProductBoot, GET/POST /api/pocket/v1/profile (identity/heap gated, CAS on
  generation), status `pocketProfile: 1`. collectOverview, the Home layout
  (weather bottom/top/off, next event), a read-only monitoring item and the
  sleep frame follow it; sleep mode `reader` delegates to SleepActivity.
  Defaults reproduce the previous behaviour. On X3: w15cb1e54 endpoint checks
  (defaults gen0, 400/409/409, save gen1 persisted across reboots), then
  w1e22398f fixed a weather-first sleep frame that filled the screen, and
  w6a915992 replaced the corner wake icon (it clipped "Powered off") with a
  full-width wake band. User confirmed Home, sleep and wake. The X3's
  startupApp was 0 (library); set to 1 via the preferences endpoint on user
  request. Installs: two transfers dropped once each and resumed once from the
  retained staging (CRC verified). Test profile left active: home study,
  reading, monitor; weather top; next event off; sleep weather, reading, study.

- 2026-09-25 P1-1 (docs/pocket-profile-v1.md): `GET /api/pocket/v1/display`
  reports the reader's resolved content-page inputs from the same helpers the
  device draws with (`currentContentPageStyle`, `ContentPresentation::pageLabels`,
  font size lookup). Dev-only `POST dev/capture` + `GET dev/frame` read the
  completed content frame in Sync. Installed wbfc03fa5 (one timeout at
  5.2MB/6.1MB, reader recovered without reset, one resume from the retained
  staging, CRC 0312E2DA). The X3 runs the classic theme (spacing 10) and draws
  "« Back"; neither the old Base nor a Lyra assumption matches every reader.
  App PocketParityTests: gen7 card with image, 0 of 418,176 pixels differ;
  "Back" label control 600. 370 host tests, default/gh_release, strict
  cppcheck pass. Decision-card relaxed for an opt-in read-only monitoring item.

- 2026-09-24 repeated Apply memory fixed on X3 dedicated Sync. Cause: stock
  WebServer actively closes every response, and lwIP kept each PCB in
  TIME_WAIT for120s (~256B, cap16). Eight status reads fell19,016->17,468B
  on wbcb431b1; heartbeat plus an Apply burst explains14,184/13,140B failures.
  Sync now purges only this server's TIME_WAIT PCBs (ports80/82) every100ms
  and on every pass while admission is pending (ServerTimeWait.*); lwIP
  tcp_abandon frees TIME_WAIT without a segment (verified in the ELF). Floors
  stay16KiB/4KiB; File Transfer unchanged. A client-closes-first variant was
  measured and removed: macOS drops the reader FIN after curl/URLSession
  close, leaving LAST_ACK PCBs ~60s. Installed w0482f45b (identical src to
  this change) via one transfer + dev flash each; same eight reads held
  19,136-19,176B, census timeWait0/lastAck0. Actual Mac app edits in one
  session: gen4 rendered at17,524B/8,180B, then second Apply gen5 rendered at
  20,428B/13,300B. These are driver receipts; per-card optical confirmation
  was not separately reported. X4, private AP, long idle and
  more repetitions remain unverified. Dev-only GET dev/tcp reports heap,
  purge count and PCB states. Evidence build/apply-memory/; design in
  docs/sync-route-memory.md. 364 host tests, default/gh_release builds and
  strict cppcheck pass.
- This host's Homebrew Python/Swift are blocked by macOS Local Network
  privacy while Apple curl/nc work; installs used a loopback relay to nc
  with pocket_put.py unchanged. Not a firmware or protocol issue.

- 2026-09-24 physical cleanup installation: wbcb431b1 installed in one
  uninterrupted wireless transfer (6,102,096B, reader CRC95643196); exact
  version verified after one developer flash. Origin marker correctly
  returned to dedicated Sync STA/poll without manual menu re-entry. Evidence:
  build/product-cleanup-install-20260924.log. Latest companion's first Apply
  of existing generation1 returned rendered/none at heap16,448B/block10,740B.
  Edited text activated generation2 but presentation failed/memory at14,184B/
  7,156B; restoring original activated generation3 and failed at13,140B/8,180B.
  Identity-bound reads confirmed original restoration; no rejoin/reboot/retry
  was used to mask failure. Later idle status freeHeap15,696B demonstrates
  some recovery, not proof of a permanent leak or its owner. Source review
  confirms upload reset drops client/buffer and presentation drops prior card
  snapshot before admission; font unload exists after drawing. The remaining
  allocation owner is not established. Keep16KiB/4KiB guards unchanged and
  distinguish storage success from repeat-redraw acceptance. AP/X4 and new
  optical confirmation remain unverified.

- 2026-09-24 product cleanup: removed 2048/Lights Out/Sokoban activity/model,
  home routing, translations and seven dedicated tests. SD games.bin and its
  backup/temp data are untouched; deleted tracked source remains recoverable
  from Git. Home menu now uses bounded stack pointer/icon arrays (max7) rather
  than per-paint vectors; OPDS/continue-reading order and base reader preserved.
  Formatting helper skips deleted tracked paths. 359 remaining host tests,
  default build and strict cppcheck pass (build/product-cleanup-*).
  Versus preceding connection-UX build, linked flash falls6,097,571→6,088,143B
  (9,428B); static RAM stays129,920B. No claim of runtime Sync heap gain.
  Frozen firmware/pocket-daily-1.6.6-default-b8e38e39-20260924-144625.bin,
  version1.6.6-dev-main-b8e38e39-wbcb431b1,6,102,096B,SHA256
  9d396609e2b9da8f6a1be0fca3a98543e7cb4737f9b8dd89fa6465eaaf869d5e.
  Subsequently installed; see physical follow-up above. Optional AgentDeck remains default-off; shared offline card
  structures/provider OTA paths need separate separation, not blanket deletion.

- 2026-09-24 user confirmed the card is visibly correct on we46ca975 after
  the recorded rendered receipt. Subsequent UI-only work names Pocket Sync
  choices Same Wi-Fi / Direct connection (English and Korean), independent
  of browser File Transfer labels. Dedicated connected view guides app use,
  removes browser QR/URL instructions and warns of direct-session internet
  interruption; original browser view and network selection behavior remain.
  Text lives in translation YAML, with no new persistent allocation/service.
  Companion labels, compatibility hints and tests updated together. New UI
  is not installed; no additional hardware request was made for this work.
  Final local checks: 366 host tests, default build and strict cppcheck pass
  (build/connection-ux-*). Frozen artifact
  firmware/pocket-daily-1.6.6-default-b8e38e39-20260924-135851.bin,
  version1.6.6-dev-main-b8e38e39-w024683da, 6,111,536B, SHA256
  396d15093b5df7740d36ed192338d566836cae0f79eadab7ab21ff6a96e60e15.

- 2026-09-24 we46ca975 physical dedicated Sync presentation passed the heap
  gate and reported rendered/none for the existing active generation1 after
  one display-only POST. Initial status freeHeap19,328B; receipt admission
  heap17,996B/block12,788B (prior waefed08b failed at14,212B/9,204B).
  No card upload, activation, reconnect or additional flash. Two immediate
  HTTP reads timed out during preparation; a later bounded receipt read
  succeeded. Optical confirmation, preparation responsiveness, repeated Apply,
  private AP and X4 remain acceptance work; rendered receipt alone is not
  visual confirmation or seamless-sync acceptance.

- 2026-09-24 user-authorized we46ca975 wireless installation verified on the
  same X3. One uninterrupted paced transfer completed 6,109,792B with reader
  CRC75EAF9A5, publication, one dev flash and exact version verification.
  No resume/retry or Mac network change. Evidence:
  build/sync-routes-install-20260924.log. Postboot active revision/generation1
  preserved. Legacy initiating marker returned to File Transfer STA/push as
  expected; requested one dedicated Sync entry before measuring route savings
  or presenting the existing card. No redraw success claimed. Crash-report
  availability in status alone does not establish a new crash; boot breadcrumb
  was dev:remote-flash with software restart.

- 2026-09-24 dedicated Sync now dispatches exact HTTP method/path pairs through
  the existing fallback callback, avoiding persistent per-endpoint Arduino
  handlers, cloned URIs and callback wrappers. Browser File Transfer retains
  registered routes; both use one route definition, and multipart /upload stays
  registered. Target DWARF reports an 80B handler object; total heap savings
  are not yet measured. Admission floors remain 16KiB/4KiB. See
  docs/sync-route-memory.md. All 366 host tests, default build and strict
  cppcheck pass; companion's 26 focused contract tests pass. Frozen artifact:
  firmware/pocket-daily-1.6.6-default-b8e38e39-20260924-130012.bin,
  version 1.6.6-dev-main-b8e38e39-we46ca975, 6,109,792B, SHA256
  546173b43de8277a3b715fdb6ca2818c5184033022c9a08f6ab6fbc505673704.
  Includes the earlier boot-return fix; neither change is installed yet.
  Actual HTTP handling, admission budget and same-session redraw remain
  physical acceptance gates; do not retransmit the already active card.

- 2026-09-24 waefed08b physical deferred-admission result in dedicated Sync:
  POST existing generation1 returned queued; GET returned failed/memory,
  heap14,212B/block9,204B. Total heap is2,172B below16,384B while the4,096B
  contiguous gate passes. HTTP deferral alone does not provide enough budget;
  preparation/font loading never began. Stored activation remained unchanged;
  no upload/reflash/assistant network mutation. Persistent session allocation
  reduction remains necessary before claiming same-session redraw success.

- 2026-09-24 fixed developer-update return routing, not yet installed. The
  flash handler persists/readbacks a one-byte origin before flashing; failure
  refuses flash. Shared-LAN Sync returns to Sync with one saved association
  attempt; File Transfer retains its own profile. AP returns to the matching
  chooser without persisting a private lease or silently joining home Wi-Fi.
  Marker consumption precedes crash/recovery routing and must remove the file
  before automatic return. Unknown/unreadable markers do not start a radio.
  Legacy `1` remains File Transfer; the first upgrade from old initiating
  firmware cannot reconstruct its missing origin. See docs/dev-update-return.md.
  All364 host tests, default build, strict cppcheck pass (build/boot-return-*).
  Frozen firmware/pocket-daily-1.6.6-default-b8e38e39-20260924-113320.bin,
  version1.6.6-dev-main-b8e38e39-we5626481,6,107,904B,SHA256
  36c488a7226d780747725e88af0784f395389a18abf42683722e6e193b49a25b.
  Physical return-mode and deferred card redraw acceptance remain pending.

- 2026-09-24 user-authorized waefed08b wireless install succeeded. Initial
  attempt timed out after1MiB logged ACK; HTTP/TCP82/ICMP were temporarily
  unresponsive. HTTP later recovered at advancing uptime without reset.
  One same-staging continuation explicitly refused RESUME0 and resumed from
  1,081,344B; reader confirmed OK6107488/954C70A6, publication, then one dev
  flash and exact1.6.6-dev-main-b8e38e39-waefed08b on the same identity.
  Logs build/deferred-install-20260924.log and
  build/deferred-install-resume-20260924.log. Mac Wi-Fi unchanged. Postboot
  File Transfer STA advertises push; active generation1 preserved. Requested
  one dedicated Sync entry before testing deferred presentation. No content
  resend; successful redraw/repeated Apply/AP/X4 remain unverified.

- 2026-09-24 deferred presentation implemented: HTTP verifies active identity
  and queues metadata; the activity waits for HTTP cleanup and upload reply
  grace before the unchanged16KiB/4KiB preparation gate. Old card snapshots
  are released at enqueue, and request/upload handling does not overlap the
  paint's font budget. No new task/framebuffer/radio transition. Schema1 adds
  optional failure/heap/block fields so deferred failures remain readable for
  the requested revision; Swift handles old/unknown reasons safely. Four new
  controller tests plus a source wiring check pass; all362 host tests, default
  build and strict cppcheck pass (build/deferred-* logs). Staged artifact:
  firmware/pocket-daily-1.6.6-default-b8e38e39-20260924-103537.bin,
  6,107,488B, version1.6.6-dev-main-b8e38e39-waefed08b, SHA256
  d6e99eb56f7c7385046081db269fcb980db255c981c8f23c1be793992c413e63.
  Not installed; actual freed heap/redraw, repeated edits, AP and X4 remain
  unverified. Do not lower the guard or claim the budget is sufficient from
  host tests. Next authorized hardware check needs only existing-card display,
  not another content upload; read failed receipt's admission sample if needed.

- 2026-09-24 dedicated COMPANION confirmed on installed w615b26dd: poll-only,
  diagnostics false,16,632B free initially. Mac applied the saved one-card
  revision and GET state confirmed generation1. Presentation remained absent.
  One subsequent display-only POST returned503 with the explicit low-memory
  admission message; status then reported11,856B free. Current handler requires
 16,384B free and4KiB largest block before prepare. Do not lower this unmeasured
  safety budget merely to force drawing; investigate resource lifetime first.
  Card storage/activation succeeded; redraw, repeat edits, direct AP and X4
  remain unverified. App fallback GET replaces the original error with409,
  obscuring this distinction. No content resend, flashing or mode change.

- 2026-09-24 user explicitly requested another wireless attempt. Reused the
  same frozen `w615b26dd` image and staging handle with attempts1/resume/10ms
  pacing. Reader returned RESUME2048000; remaining bytes completed with
  `OK 6106576 4E2DDDCE`, verified publication and one dev-flash request.
  Exact new version `1.6.6-dev-main-b8e38e39-w615b26dd` was verified on the
  same identity; subsequent status showed uptime15/dev:remote-flash/software
  restart. Log: build/sync-install-resume-20260924.log. Mac Wi-Fi unchanged.
  Boot returns to File Transfer STA (push/diagnostics still advertised), not
  the dedicated Sync profile. Next hardware step is Pocket Daily → Sync →
  Join a Network, then same-session card/redraw testing. Installation succeeded;
  dedicated Sync stability, direct AP and X4 hardware acceptance remain open.

- 2026-09-24 user approved one wireless installation of `w615b26dd`.
  The frozen 6,106,576-byte image (SHA256
  `6b673a26a5ac8b710b0eb1c73659e1d4f4b7a5fd6ffc865aa5232d68c982a736`)
  was sent with resume,10ms pacing and attempts1. Last printed SD ACK was
  1,835,008 bytes; socket timeout ended the attempt before publication or flash.
  A single subsequent status read succeeded on the same identity, still
  `w6fafa273`; uptime advanced, no reboot indicated. No resend/reflash. Log:
  build/sync-install-20260924.log (private/ignored, includes resume handle).
  The new Sync changes remain uninstalled. A one-time SD bootstrap is the
  bounded alternative to another blind wireless attempt; no card write yet.

- 2026-09-24 dedicated Sync resource policy now keeps both COMPANION (shared
  Wi-Fi) and POCKET_SYNC (BLE/private AP) poll-only on X3/X4. Listener startup
  and resumption refuse those profiles regardless of heap; status skips optional
  SD diagnostics/preview advertisement. Explicit diagnostic routes remain.
  Theme list/apply handlers were incorrectly gated with frame streaming and
  absent on private AP despite uiPacks:true; they now register unconditionally.
  Sync chooser/connected headers use the existing localized Sync label instead
  of File Transfer. No new buffer/task, radio tuning, protocol grammar, or
  automatic flash behavior. Heap/HAL skills preserve native storage/rendering.
  All 358 host tests (including route-scope regression), default build and strict
  cppcheck pass: build/sync-session-{host,firmware,check}-final.log. Prepared
  artifact: pocket-daily-1.6.6-default-b8e38e39-20260924-011105.bin,
  6,106,576 bytes, embedded version 1.6.6-dev-main-b8e38e39-w615b26dd.
  Not installed or hardware-validated. The prior intermittent TCP/partial HTTP
  failure is not proven fixed; run the companion docs/SYNC_SESSIONS.md hardware
  gate without repeated blind uploads. App accepts the unchanged poll/diagnostic
  advertisement and now directs users to Pocket Daily → Sync, retaining File
  Transfer as compatibility recovery and explicit-only private Wi-Fi switching.

- 2026-09-23 explicit user-authorized X3 wireless installation completed from
  sta-recovery w998f2f9d to default `1.6.6-dev-main-b8e38e39-w6fafa273`.
  The6,106,496-byte staged image SHA256 is
  `9412bdc2f6963cdbee953149bf98082eae0e391d9f9b96274fd985456b75f3f5`.
  One pocket_put attempt with resume and10ms flow pacing received matching
  `OK 6106496 7B2B0410`, published /update.bin, requested dev flash once and
  verified the exact installed version on the same identity. Status reports
  software restart/dev:remote-flash, STA and contentPresentation=true; content
  state returns HTTP200/schema1/capabilities7/active:null (previously404).
  Log: build/device-install-20260923.log. Mac Wi-Fi was not changed; Pocket
  was temporarily quit to avoid competing traffic and then reopened/connected.
  This proves this installation/API availability, not content redraw, X4,
  long-idle reliability or a production radio fix. No content was applied.

- 2026-09-23 content navigation now sends distinct localized previous/next
  labels through the existing MappedInputManager instead of repeating Prev/Next
  in both slots. English and Korean source keys added; font preflight checks
  both actions. A production-controller test pins labels and preflight coverage.
  All356 host tests, default build and strict cppcheck pass
  (`build/content-hints-{host-final,firmware,check}.log`). HAL/heap constraints
  retain existing mapping/rendering and add no buffer or runtime allocation.
  The app matches these reference labels and verifies actual bundled-font
  pixels; shared renderer/ABI/artifact unchanged. No device contact or flashing.

- 2026-09-22 the unused whole-volume AllocationScan, geometry/allocation HAL
  extensions and their SdFat/SDK patches were retired. Per-cluster stepping
  still scales with card size and has no production caller/reservation; installed
  SdFat does not maintain a warm free count, and contiguous preallocation failure
  cannot be treated as a full card. Keep the existing allocator fallback,
  atomic publication/activation and bounded revision-directory validation.
  Only the newly introduced unused layer and its dedicated tests were removed;
  the original format-only SDK patch remains. Local recovery archive:
  build/retired-allocation-layer.tar. No device contact or network change.
  This supersedes earlier scanner/geometry records and does not complete
  full-card failure handling, quota/orphan handling or physical acceptance.
  Verification:355 remaining host tests, default build, strict cppcheck and
  three format-patch tests pass (`build/storage-simplification-{host,firmware,check}.log`).
  The seven removed C++ tests belonged only to the retired layer; production
  transfer/content/activation/recovery tests remain. HAL/heap constraints retain
  the existing storage lock and allocator rather than adding another preflight.

- 2026-09-22 duplicate staging cleanup can resume on a later explicit seal
  after partial deletion. PreparedRevision now counts verified asset bytes/files
  internally; no wire field changes. After full published verification, cleanup
  requires the matching staged manifest and exact inventory of that verified
  subset. Missing already-deleted assets are allowed; corrupt/unreadable/extra
  files prevent deletion. Manifest-only remainders work; missing manifests still
  prevent reclamation. No timer, global scan or new allocation; HAL/heap bounds
  remain.355 host tests/default build/strict cppcheck/format pass
  (`build/staging-cleanup-resume-{host,firmware,check}.log`);32 Swift contract
  tests pass (`.build/staging-cleanup-resume-contract.log`). No reader contact,
  upload or physical cleanup. General quota/reservation/orphan collection remain
  open; this is not power-loss or real-media acceptance.

- 2026-09-22 idempotent seal now reclaims an exact, fully verified duplicate
  staging tree after independently verifying the existing published revision.
  Previously this branch returned success with the duplicate still present;
  the before-fix regression fails (`build/staging-duplicate-before.log`). Cleanup
  uses manifest-listed paths and non-recursive rmdir, preserving unknown,
  incomplete or corrupted staging and never modifying published/active records.
  Mid-delete failure keeps successful published status and does not retry partial
  cleanup. HAL/heap skills reused the existing <=272B transient deletion workspace
  with no persistent buffer; caller writer exclusion/watchdog rules are unchanged.
  Final354 host tests, default build, strict cppcheck and formatting pass
  (`build/staging-duplicate-{host-final,firmware-final,check}.log`). Companion32
  deployment/adapter/HTTP tests pass (`.build/staging-duplicate-contract.log`).
  No wire/Swift source change, reader contact or physical deletion. This is one
  accumulation-path fix, not aggregate quota/free-space reservation or durable
  orphan cleanup. See content-storage-admission.md.

- 2026-09-22 PDCT v2 carries ImageFirst/SideBySide in byte491; v1 text-first
  output is unchanged. PDCM capability4 is exact-checked against decoded cards
  during verification; runtime state advertises7 and native content view loads
  with all supported bits. Shared ContentPageRenderer draws these presets using
  oriented metrics and existing image callbacks, no new heap/frame buffer/task.
  HAL/heap skills preserved the existing allocation bounds.350 host tests,
  default build/strict cppcheck pass (`build/card-layout-{host-final,firmware,check}.log`);
  real-font48 mode/40 ABI frame comparisons pass (`build/card-layout-pixels.log`).
  `apple-host-0pvcfof1` was verified/imported by the sibling app;236 Swift tests
  and macOS build pass. App picker/draft/preview/deployment share these bytes,
  with pre-staging capability refusal and verified-image reuse. No reader
  contact/upload or physical parity claim. See content-card-layout-v2.md;
  arbitrary UI trees, other surfaces and product/security acceptance remain.

- 2026-09-22 ContentSealStore now requires exact flat inventory after full
  manifest/asset verification, before rename, on published retries and after
  rename. Limits are manifest files+1, exact logical bytes, zero subdirectories,
  <=512 raw reads including end; watchdog-only progress retains writer exclusion.
  Failures preserve candidate/previous active state and never silently remove
  leftovers. Existing SealResult/HTTP422 contract stays unchanged. HAL/heap skills
  kept transient bounded accounting and one scoped directory handle.348 host
  tests, default build and strict native cppcheck pass
  (`build/content-inventory-{tests-final,firmware,check}.log`);25 companion
  revision/deployment/adapter tests pass. Fake FAT-tree integration is not a real
  volume/server test. Aggregate pre-upload quota/free space/reservation still
  remain open. No reader contact/upload. See content-storage-admission.md.
- 2026-09-22 DirectoryAccounting adds a <=128B allocation-free flat-directory
  state machine over one-slot HAL reads: caller limits raw work/files/children/
  logical bytes; FAT LFN linkage and exFAT set sequence/checksum/lengths are
  checked. Invalid, unsupported, I/O and limit states never publish partial
  totals;64-bit addition is overflow-safe. HAL/heap skills kept locking below
  the decoder and no names/dynamic inventory buffers.344 full host tests,
  default build and strict native cppcheck pass
  (`build/directory-accounting-{tests-final,firmware,check}.log`). Fixtures cover
  structure/accounting, not complete filename/cluster validation or real SD.
  Not wired to upload admission yet: recursive staging policy, mount/write
  ownership and free-space/reservation remain open. No app wire change or device
  contact/upload. See docs/content-storage-admission.md for exact limitations.

- 2026-09-22 HAL filesystemFormat now exposes mounted FAT/exFAT/Unavailable
  metadata under the storage mutex without scans or allocation. A reviewed
  two-line SDK accessor patch is applied idempotently by the base pre-script;
  the SDK pin is unchanged, its working header is patched, and incompatible or
  missing patch inputs fail before mutation. Keep scripts/storage_sdk.patch,
  patch_storage_sdk.py and platformio.ini together with the HAL consumer.
  Three Python patch tests and337 full host tests pass; default build and strict
  native cppcheck pass (`build/filesystem-format-{tests,firmware,check}.log`).
  HAL/heap skills kept SDK access inside serialized storage. This is a format
  query only, not capacity admission or a remount-safe snapshot; decoder,
  aggregate quota/reservation and physical acceptance remain open. No app wire
  change, reader access or firmware upload. See content-storage-admission.md.

- 2026-09-22 HAL now exposes allocation-free readDirectoryRecord: one raw32B
  slot per locked SDK read, including deleted/LFN entries, with Record/physical
  End/Error and cleared failure output. Invalid/alignment/sticky/new-read-error
  cases cannot masquerade as clean EOF. Existing openNextFile is unchanged.
  HAL/heap skills kept SD serialization and no added persistent buffer.336 full
  host tests pass (`build/directory-record-tests-final.log`); default firmware
  build and strict native-tool cppcheck pass (`build/directory-record-{firmware,check}.log`).
  This is only a raw cursor: no semantic filename/entry-set validation, quota,
  free-space query/reservation, physical latency or SD-image proof. Not wired to
  admission yet; higher-level decoding/work budget must come first. No app wire
  change or reader contact/upload. See docs/content-storage-admission.md.

- 2026-09-22 capacity-admission source audit found a second prerequisite beyond
  bounded FAT free-space scanning: current HAL directory iteration conflates
  allocation failure/SDK stop with EOF. Installed SdFat openNext can fail for a
  corrupt LFN or cached-entry open without a parent read-error flag and can scan
  many deleted entries internally. Do not derive exact quota inventories by
  counting returned handles or only checking parent getError. The required HAL
  primitive must bound raw work and distinguish confirmed end from incomplete
  or corrupt traversal; see docs/content-storage-admission.md. HAL/heap skills
  guided the audit. No runtime change, capacity claim, SDK patch or reader access.

- 2026-09-22 staging ingress now enforces the existing256KiB content ceiling per
  incoming file, with case/segment-aware path matching and overflow-safe resumed
  byte accounting. Port82/legacy WS reject declared excess before file mutation;
  HTTP/WebDAV check each chunk, and commit/browser move/rename/WebDAV MOVE/COPY
  check destination admission. WebDAV directory moves into staging are refused.
  Ordinary books/firmware/UI-pack paths are unaffected; no wire-schema change.
  HAL/heap skills kept hot-path checks allocation-free (one HTTP state Boolean),
  with scoped HAL admission for WebDAV relocation.332 host tests, default build
  and strict cppcheck pass (`build/content-ingress-{tests,firmware,check}.log`);
  companion15 manifest/revision/adapter tests pass. Handler integration was
  source-audited/compiled, not live-server tested. No reader contact/upload.
  This is not aggregate quota/free-space admission: the SDK exposes no space
  query and underlying freeClusterCount may scan the full FAT synchronously.
  No unbounded scan was added to the radio request path. Bounded space queries
  or reservation, orphan/staging cleanup and hardware acceptance remain open;
  see docs/content-storage-admission.md.

- 2026-09-22 content activation now returns its evicted slot revision only after
  successful readback. The endpoint performs best-effort bounded retirement,
  protecting both directly opened/CRC-valid active records and every non-idle
  presentation revision; unknown presentation or uncertain metadata refuses it.
  SHA/CRC-validated manifest-listed assets only are removed, then the closed
  manifest and non-recursive empty directory. Unknown descendants survive;
  partial cleanup never changes activation success. HAL/heap skills kept the
  transient workspace <=272B and existing serialized SD access, no inventory or
  persistent buffer.330 host tests pass, including20 sequential deployments,
  fallback/pins/faults/partial retry (`build/content-retirement-tests-final.log`).
  Default build passes (`build/content-retirement-firmware-final.log`), strict
  cppcheck passes with the existing local-tool override (`build/content-retirement-check-local.log`);
  the normal final check was stopped during repeated package-mirror failures.
  Companion28 deployment/receipt/presentation tests pass; response schemas and
  renderer algorithms/ABI are unchanged. The previously imported host package
  remains pinned to its historical source snapshot, not this changed device
  storage tree; its artifact verification passes. No reader access/upload or
  physical deletion. Durable retirement retry, orphan/staging collection, total
  quotas/free-space admission and hardware acceptance remain open. See
  docs/content-retention.md.

- 2026-09-22 companion imported `apple-host-sai3hncw` through the approved
  host-artifact exception, with source/artifact pinning and sandboxed Xcode
  verification. Its actor-isolated Swift bridge executes the actual renderer
  in iOS simulator tests (PDCT/PBM, empty pages, rotations, refusals/retry and
  cancellation); full202 app tests, macOS and unsigned iPhoneOS builds pass.
  No firmware source/device image/font was copied and no reader accessed.
  This is app interop, not physical parity or preview UI completion. See
  sibling docs/HOST_RENDERER.md; the packaged ABI/source tree is unchanged.

- 2026-09-22 `host/build_apple.py` builds a three-platform XCFramework with five
  architecture slices (macOS14 arm64/x86_64, iOS17 arm64, simulator arm64/x86_64).
  Each slice links the actual Swift consumer. macOS packaged consumer rendered
  Korean/copied52272 bytes and rejected stale readback. Provenance schema1 JSON
  pins commit plus dirty/untracked source manifests, compiler dependency closure
  (including MiniBidi .t tables), build/SDK metadata and package hashes; six
  script tests cover drift, unsafe paths, incomplete slices and dependency gaps.
  Verified output: `build/apple-host-sai3hncw`; sources/package re-verification
  and326 existing host tests pass (`build/apple-host-{build,tests}.log`). No
  overwrite, device access or app import; provenance is integrity, not a signature.
  App sync/bridge/UI and physical/iOS execution remain pending. HAL skill kept
  only host artifacts in scope; device code/heap are unchanged.

- 2026-09-22 independent `host/` CMake target builds `libpdui_host.a` without
  test adapters/GoogleTest. Public PocketUIHost.h/module map expose version1
  content-page create/render/info/copy/destroy; contexts own bounded font bytes,
  validate PDCT/PBM/UTF-8 and hide prior pixels after any failed request. Rendering
  is globally serialized at the C boundary: source inspection found MiniBidi's
  static line scratch, so thread-local assets alone are insufficient.326 host
  tests pass, including C consumer, synthetic-font lifetime/refusal/recovery and
  independent-context calls.32 real-font reference frames plus24 C ABI page
  comparisons pass; macOS Swift imports/renders/copies Korean output successfully.
  Standalone build, strict firmware cppcheck/default build pass
  (`build/host-abi-{tests,fixture,swift,standalone,check,build}.log`). HAL skill
  kept device algorithms/storage untouched; no reader access or artifact copied
  into the app. Native theme surfaces, ABI UI-pack apply, Apple packaging,
  app bridge/UI and physical parity remain incomplete. See `host/README.md`.

- 2026-09-22 host rasterization no longer depends on mutable FakeSD globals.
  `host/hal/HalStorage.h` provides borrowed immutable assets, scoped thread-local
  lookup, bounded reads/seeks and no write/OS-file API. Nested scopes restore
  bindings; open handles snapshot views and keep independent cursors. Owners
  retain asset bytes and serialize their own renderer.319 host tests and32
  real-font frame comparisons pass (`build/host-assets-{tests,fixture}.log`).
  Fixture reads are capped to the measured64MiB maximum, including file growth
  during loading. Device storage is untouched; the host C ABI/Swift bridge and
  artifact packaging are still pending, not implied by this adapter.

- 2026-09-22 ContentImageRenderer now shares BaseTheme's PBM rasterization,
  bounded clipping and partial-output clearing with the host page renderer.
  No added device allocation or display call. Independent pixel-oracle and
  late-read-failure tests cover both geometries/all four orientations, preserving
  outside pixels and buffer guards.315 host tests and32 real-font
  text/card/empty/image frame comparisons pass; strict cppcheck/default build
  pass (`build/content-image-host-{tests,fixture,check,build}.log`). This extends
  the earlier24-frame baseline below; C ABI, Swift bridge, other native surfaces
  and physical parity remain incomplete. No reader requests or uploads.

- 2026-09-22 extracted ContentPageRenderer used unchanged by device BaseTheme
  and host. It owns title/body/footer/image placement; caller supplies theme
  values/translations/preflighted font and image callback, retaining HAL/storage
  ownership. No added framebuffer, radio or display call.312 host tests, strict
  cppcheck and default build pass (`build/content-page-host-{tests,check,build}.log`).
  Real PocketSansWorld12 comparison passes24 text/card/empty frames across two
  geometries/four orientations (`build/content-page-host-fixture.log`). Initial
  mismatch was fixture misuse of Cached prewarm: per-field calls replace its
  page mini-kerning matrix; reference now correctly prewarms the full page once.
  HAL/heap skills kept the runtime adapter allocation-free. Composed image
  pixels, other native surfaces, C ABI/Swift bridge and physical goldens remain
  unverified/unimplemented. No reader access or upload.

- 2026-09-22 host raster foundation `test/gfx_host` compiles real GfxRenderer,
  bitmap/dither, font/cache/decompression and MiniBidi sources with a bounded
  memory-display HAL and existing fake SD. Local PocketSansWorld12 fixture
  rendered8 nonblank byte-identical Cached/BoundedUI frames (2 geometries x4
  orientations, regular/bold Latin/Korean/Japanese), canaries intact
  (`build/gfx-host-fixture.log`). Real-font preflight exposed newline controls
  as missing glyphs: shared checkLayoutText now normalizes layout whitespace in
  bounded UTF-8 chunks; wrapping renders tabs as spaces.309 host tests, strict
  cppcheck and default firmware build pass (`build/gfx-host-{tests,check,build}.log`).
  HAL/heap skills isolate host buffers from firmware; hardware-only blit/gray
  paths fail explicitly. No device access. Theme/page host surfaces, C ABI,
  Apple artifact packaging/Swift bridge and physical pixel parity remain absent.
  Production-source unused-parameter warnings in the broader host build remain
  visible, not globally suppressed. See `test/gfx_host/README.md`.

- 2026-09-22 live content page now uses shared ContentPageLayout before clearing
  the framebuffer. Widened arithmetic rejects overflowing/oversized metrics,
  invalid viewports and nonpositive line heights; empty-state minimum height
  includes its previously unreserved title/body gap. Body line counts and image
  height share remaining-space calculations. Four new geometry tests cover
  X3/X4 dimensions, rotated insets, minimum height and int32 extremes.304 host
  tests, strict cppcheck and default build pass
  (`build/content-layout-{tests,check,build}.log`). HAL/control-flow skills kept
  oriented metrics and early refusal; no new allocation or device access.
  This verifies line boxes, not glyph ink/pixels, and does not complete the host
  renderer or declarative UI runtime. See `docs/content-display.md`.

- 2026-09-22 failed live-content framebuffer export is now gated. ActivityManager
  checks canCaptureFrame before live publication; manual screenshots check the
  same activity permission under RenderLock before BMP/border output. File
  Transfer requires completed native/content drawing and refuses queued/failed
  content, including the gap after hiding a damaged view before repaint. Other
  native activities retain legacy policy.300 host tests, strict cppcheck and
  default build pass (`build/content-capture-{tests,check,build}.log`). Controller
  capture transitions are tested; manager/screenshot scheduling was source
  inspected, not hardware-tested. No extra framebuffer or network protocol
  change; HAL skill preserved the shared renderer/lock boundary. No deployment.

- 2026-09-22 fixed live-view Back handling that returned before handleClient
  while waiting for a render. ContentSessionInput now separates deferred view
  dismissal from explicit session exit; idle/drawing input decisions continue
  the server loop, and a consumed Back edge cannot fall through to exit.
  Four edge-sequence tests plus the full299 host tests, strict cppcheck and
  default build pass (`build/content-input-{tests,check,build}.log`). Control-flow
  and HAL skills kept exhaustive action dispatch and mapped input; no new
  allocation or network mode change. Source integration inspected; actual
  activity scheduling/latency on hardware remains unverified. No device access.

- 2026-09-22 production ContentPresentation controller now has seven host tests
  with fake storage/font/theme/display boundaries. They cover exact revision
  receipts, queued-until-driver-return, writer-gate/font release, load/OOM,
  missing glyph/read/paint failures, navigation wrap, empty content, hide and
  explicit-only recovery.295 host tests, strict cppcheck and default build pass
  (`build/content-controller-{tests,check,build}.log`). HAL/heap skills kept the
  test seam outside production: no added runtime buffer or test-only firmware
  branch. Actual theme pixels, font-facade allocation failures, activity-level
  scheduling and physical acceptance are not proven. No device access/upload.

- 2026-09-22 local live content presentation is hosted inside File Transfer:
  exact verified revision load, BoundedUI coverage preflight, shared theme page,
  queued/rendered/failed receipts and font release before the next transfer.
  Back returns to transfer UI; no session-end/reboot accompanies Apply.
  Swift uses one POST then read-only verification and preserves activation on
  redraw failure.288 host tests, strict cppcheck and default build pass
  (`build/content-presentation-{tests,check,build}.log`); app23 focused tests and
  both platform builds pass. Render-controller fault injection, captured-frame
  parity and physical heap/display/SD acceptance remain unverified. No device
  access or deployment. See `docs/content-display.md` for the contract.

- 2026-09-22 opt-in `SdCardFont::BoundedUI` keeps interval/shaping tables on SD
  and uses the existing glyph path with <=2048B retained bitmap payload plus
  <=256B replacement (object/HAL/allocator/renderer costs excluded). Cached
  remains default; the live presenter now selects the mode as noted above. Disk-backed
  kerning/ligature callbacks preserve shaping rather than dropping it. Read,
  allocation and invalid-bitmap failures latch until reload.284 host tests,
  strict cppcheck and default build pass (`build/bounded-font-{tests,check,build}.log`).
  Local bundled PocketSansWorld12 fixture compared71,036 glyph metrics/bitmaps
  and sampled shaping against Cached (`build/bounded-font-fixture.log`). Not
  panel, physical heap or SD latency evidence. HAL/heap/control-flow skills
  preserved locked storage, fallible cache replacement and opt-in behavior.
  See `docs/bounded-ui-fonts.md`; live presentation and Swift receipts remain
  unfinished. No reader contact or firmware upload.

- 2026-09-22 display data is separated from activity/network lifetimes in
  `ContentViewState`, now used by the existing Pocket activity. One on-demand
  <=2400B card snapshot is reused on reload; revision/generation publish only
  after full validation, failures clear all state, empty differs from no-active.
  Exact-target loads reject recovery to a different older revision; offline
  loads retain normal fallback.276 host tests, strict cppcheck and default build
  pass (`build/content-view-{tests,check,build}.log`). HAL/heap/control-flow
  skills retained locked storage, fallible allocation and explicit load outcomes.
  This is the shared model for a future lightweight live-session view, not a
  completed display endpoint. Existing File Transfer exits restart the device;
  do not repurpose them for every content Apply. No physical device access.

- 2026-09-22 preparation now fails explicitly on reuse destination creation,
  chunk copy or readback failure (`CopyFailed`, HTTP503), rather than reporting
  missing assets and inducing another upload attempt. Receipt fields stay empty;
  published content is unchanged. Source absence/hash mismatch remains optional
  reuse.272 host tests, strict cppcheck/default build and28 Swift contract tests
  pass (`build/content-copy-failure-*`, sibling `.build/content-copy-failure-*`).
  No free-space scan added: installed SdFat's uncached FAT traversal is not a
  bounded preparation operation. Space admission/retention remains pending.
  HAL/heap skills kept existing locked I/O and bounded scratch; no device access.

- 2026-09-22 content preparation now best-effort copies same-path/size/SHA
  assets from the recovered active revision into missing staging files, using
  exclusive creation,64B chunks and candidate readback hashes. Existing files
  and active records are never overwritten by reuse; interrupted/corrupt copies
  remain unverified and require normal upload. Read-only inspection remains
  available internally. Schema1 receipt unchanged; Swift skips verified copies.
  271 host tests, strict cppcheck/default build and26 app contract tests pass
  (`build/content-reuse-*`, sibling `.build/content-reuse-*`). HAL/heap skills
  kept copying within storage locks, existing272B workspace and up to three
  transient file handles. No device contact/upload or measured speed claim.
  See `docs/content-prepare-v1.md`; older no-copy notes are superseded.

- 2026-09-22 canonical PBM images now render in app-card detail via the shared
  BaseTheme/GfxRenderer path. Activity pins the loaded revision ID; HAL opens
  only its canonical image path.64B row scratch, nearest-neighbor fit without
  enlargement, bounded oriented drawing area; late read failure clears partial
  output. No image/framebuffer allocation; image paints do use transient HAL
  handles/SD reads.268 host tests, strict cppcheck and default build pass
  (`build/content-image-render-{tests,check,build}.log`). See
  `docs/content-display.md` for immutable-source and physical-validation limits.
  App import/preview, retained-frame images and screen receipts remain pending.
  No device access/upload; this supersedes the text-only detail limitation.

- 2026-09-22 active app-card text now loads on Pocket activity entry through
  full revision verification, in canonical order, into existing overview/detail/
  personal-snapshot rendering. Failure clears all decoded output; active-store
  recovery can select the older verified revision. One <=2400B activity-owned
  snapshot, no per-frame I/O/allocation; local Back never queues provider actions.
  PBM drawing and automatic post-transfer repaint/confirmation remain pending.
  See `docs/content-display.md`.264 host tests, strict cppcheck and default
  build pass (`build/content-display-{tests,check,build}.log`). No device access.

- 2026-09-22 content state/activate HTTP handlers are wired under writer/identity/
  heap admission. State fully recovers stored content; activation seals then
  commits, with watchdog progress during verification. Invalid active records
  are errors, not empty state. Companion client/adapter exists; older notes
  saying these endpoints are absent are superseded. Stored selection still
  does not certify display. This code is covered by the local checks above,
  not live HTTP or physical power-cut acceptance.

- 2026-09-22 `POST /api/pocket/v1/content/prepare` now checks reader/revision,
  rejects live writers/low-memory admission, verifies the already-staged
  manifest and reports actual candidate asset size/SHA matches as a canonical
  bitmask. Missing/unreadable/mismatched assets are not acknowledged. Read-only,
  <=272B temporary workspace; hash progress feeds only registered watchdog tasks.
  Companion decoder validates matching revision/identity/count/mask.259 host
  tests, strict cppcheck/default build (`build/content-prepare-*`) and27 Swift
  content tests/iOS test/macOS builds pass.
  See `docs/content-prepare-v1.md`: activation, old-file copying, HTTP adapter
  and UI integration remain absent; no live HTTP/device validation or deployment.

- 2026-09-22 HalStorage's four file-wrapper allocation paths now check the
  existing nothrow helper before SDK opens/writes/directory iteration. OOM
  returns empty/false; empty handles have safe failure/cleanup results. Existing
  handle lifetime and recursive SD locks remain. Production HAL host tests with
  SDK/helper stubs cover allocation failure before mutation/cursor advance,
  locked replacement/destruction and empty handles.256 host tests, strict
  cppcheck and default build pass (`build/hal-allocation-{tests,check,build}.log`);
  see `docs/storage-allocation.md` for hardware/allocator limits.
  No firmware upload or device access.

- 2026-09-22 content activation uses exclusive creation for a target classified
  as missing, never truncation. Fault tests hide slot0/newest slot1/both from
  exists() while open still sees them: activation fails and preserves every byte.
  This hardens false-negative absence handling but is not a general SD-error
  classifier or power-cut guarantee.251 host tests, strict cppcheck and default
  build pass (`build/content-exclusive-{tests,check,build}.log`). No device access.

- 2026-09-22 server writer admission now gates HTTP/legacy WS/stream starts,
  generic file mutations, WebDAV mutations and commit. Rejected multipart/raw
  callbacks retain rejection through END/ABORT without changing the prior
  receipt or deleting its file. Stream REPLIED permits commit after close.
  This relies on the existing single-loop dispatch, not a new inter-task lock.
  See `docs/content-server-ownership.md`; content endpoints remain unwired.
  Verification:249 host tests, strict cppcheck and default build pass
  (`build/transfer-admission-{tests,check,build}.log`); live simultaneous
  request/hardware behavior remains unverified. No device access.

- 2026-09-22 internal ContentSealStore verifies the separate staged directory,
  renames through HAL and re-verifies publication. Existing published revisions
  are never replaced; retries verify their contents, including ambiguous moves.
  Active records remain unchanged until explicit activation. No extra heap
  workspace.247 host tests, strict cppcheck and default build pass
  (`build/content-seal-{tests,check,build}.log`); see `docs/content-seal-store.md`
  for unimplemented server ownership, quotas and filesystem-failure limits.
  No endpoint/capability or hardware deployment was added.

- 2026-09-22 generic network mutations now share ContentPathPolicy: published
  content and active-record paths/ancestors are protected across HTTP, stream,
  WebSocket (including abort cleanup) and WebDAV. Case variants and ambiguous
  FAT paths are rejected; ordinary tilde filenames are also deliberately denied.
  Future candidate transfer uses a distinct content-staging tree; sealing and
  serialized activation remain unwired. Policy has no allocation.241 host tests
  and strict cppcheck/default build pass (`build/content-path-{tests,check,build}.log`); handler
  runtime/FAT alias behavior is not hardware-verified. No device access.

- 2026-09-22 internal ContentActiveStore adds76-byte CRC/generation records in
  two slots. Recovery verifies full revisions; commits protect the recoverable
  slot, handle idempotency and refuse generation wrap. Failed readback is
  ambiguous and requires recovery. Not connected to boot/UI/network. See
  `docs/content-active-store.md`: generic write exclusion and HAL exists/error
  classification still need hardening before exposure; no hardware deployment.
  Verification:236 host tests, strict cppcheck and default build pass
  (`build/content-active-{tests,check,build}.log`).

- 2026-09-22 ContentRevisionStore now reads candidate revisions through HAL,
  validates real manifest/file SHA-256 plus card/image semantics and references,
  rejects duplicates/missing/unused images and leaves storage unchanged. Full
  golden matches Swift builder. One transient workspace bounded below1536B;
  no runtime caller/activation yet. See `docs/content-revision-store.md` for
  write-exclusion requirement, host crypto adapter and unverified hardware scope.
  Verification:229 host tests, strict cppcheck/default build; companion14 tests
  and iOS test/macOS builds pass. Logs `build/content-store-*`. No deployment.

- 2026-09-22 P3 image validation: bounded canonical P4 parser, 64-byte row scratch,
  no allocation, exact raster size and zero padding. Companion now assembles
  card/image manifests with complete references and local changed-file planning.
  Contract `docs/content-image-v1.md`, shared9x2 golden. PBM rendering, device
  file verification/activation and capability remain unwired; no device access.
  Verification:223 host tests, strict cppcheck/default build and companion13
  content tests/iOS test/macOS builds pass (`build/content-image-*`).

- 2026-09-22 PDCT v1 card codec added alongside Swift encoder; fixed512 bytes,
  strict UTF-8/canonical padding, existing Card text limits, no action payload.
  Decoder uses caller-owned output and clears it on all failures. Reads <=192B,
  no allocation. Contract `docs/content-card-v1.md`. Not wired to provider pool,
  UI/actions, activation or capability advertisement; image semantics still pending.
  Verification:218 host tests, strict cppcheck/default build, companion8 tests
  and iOS test/macOS builds pass (`build/content-card-*`). Uncalled codecs remain
  absent from the default linked ELF (nm); integration heap cost is not yet measured.

- 2026-09-22 P3 foundation: PDCM v1 structural validator and Swift encoder share
  a golden manifest. Parser is allocation-free, bounded to16 files/1684 bytes,
  rejects unsafe/duplicate/out-of-order paths and unsupported capabilities.
  See `docs/content-manifest-v1.md`. No active-content capability, file-semantic
  validation, activation, editor or deployment yet; no installed-state change.
  Verification:212 host tests, strict cppcheck and default build pass; companion
  four contract tests/iOS test build/macOS build pass. Logs `build/content-manifest-*`.
  Codec has no call site yet and is absent from the linked default ELF (nm);
  runtime cost must be measured when activation integrates it.

- 2026-09-22 offline deck publication now writes alternating v7 CRC/generation
  slots instead of deleting the current file before rename. Saves verify exact
  readback, refuse unreadable-slot overwrite and generation wrap. Loads recheck
  CRC and fall back; v6 inputs remain untouched. 128-byte validation scratch,
  no additional Snapshot allocation. See `docs/deck-cache-v7.md` for ambiguous
  readback failures, downgrade/stale fallback and SD/FAT power-failure limits.
  This supersedes the prior local deck-store replacement defect, but is not the
  app's planned multi-file content-revision protocol. No installation performed.
  Verification:205 host checks (12 actual deck-store fault-injection cases),
  strict cppcheck and default build pass; `build/deck-store-{tests,check,build}.log`.

- 2026-09-22 content invalidation: repaint and deck persistence now share
  bounded semantic CardSignature (all card fields and active choices, order,
  count). Previously title/choice-only edits could evade both checks, especially
  without a feed signature. No wire or disk schema change, no new allocation.
  Five regression cases cover fields, unused bytes, boundaries, order/removal,
  malformed counts and unterminated strings. No device deployment. Separate
  remaining storage defect: deck_store removes the prior file before rename;
  its header no longer claims power-cut-safe preservation.
  Verification:193 host tests, strict cppcheck and default build pass
  (`build/card-signature-{host,check,build}.log`). Physical repaint/reboot
  behavior remains unverified; installed baseline is unchanged.

- 2026-09-22 benchmark fidelity: future CLI payloads use versioned deterministic
  SHAKE256 data, replacing repeated0..255 blocks that could hide equal-size
  block duplication/reordering.24 tests pass, including actual readback-SHA
  rejection of those corruptions. Log `build/nonrepeating-payload-tests.log`.
  No new physical run; previous hardware evidence used the repeating pattern
  and must not be treated as arbitrary-content fidelity sign-off.

- 2026-09-22 resource-lifetime source audit excludes listener restart *inside*
  a synchronous download handler: stream→HTTP→WS→live tick ordering, with
  resume only in tick after focus cooldown/heap admission. Between-request
  five-second cooldown is still not an explicit transaction lease. See
  benchmark document. No code/install/device changes; optional memory pressure
  and radio/root cause remain unproven.

- 2026-09-22 consolidated local regression:188 CTest checks and42 Python tests
  pass (`build/current-cross-repo-host.log`, `build/current-host-tools.log`);
  companion103 unit/integration and7 UI tests pass. Live Studio contract now
  distinguishes implemented vs planned features, actual16KiB admission and
  15s app heartbeat, nonpersistent HTTP pages and uninstalled radio correction.
  No firmware code/build/install or hardware test in this documentation audit.

- 2026-09-22 per-download SO_RCVBUF experiment is opt-in host tooling only;
  22 tests pass. Requested4096 yielded OS effective35900, so a small-window
  condition was not established. 16KiB upload/commit passed but ACK max11.0383s
  preceded the option's application; readback returned10240 correct-prefix
  bytes then EOF10.8533s. Cleanup passed. See benchmark doc/log
  `build/frozen-baseline-rcvbuf4k-16k.log`. No firmware/network changes; neither
  a small-window fix nor exclusively outbound-HTTP root cause is proven.

- 2026-09-22 final prefix observation on frozen firmware: 16 KiB upload/CRC/
  commit passed; download declared16384, delivered6144 bytes matching the
  original prefix, then timeout41.2737s. Cleanup passed. Earlier8192 differs,
  so do not infer fixed8KiB truncation. `build/frozen-baseline-prefix-16k.log`.
  Stop identical repetitions; next experiment requires a causal variable.
  No reinstall/reconnect. Resource/backpressure and SD read failure remain
  hypotheses, not an established root cause.

- 2026-09-22 read1 observation: another frozen-baseline 16 KiB upload/CRC/commit
  passed, but HTTP readback returned only 8192 bytes (8 reads), then EOF at
  11.7182 s. Cleanup passed. Log `build/frozen-baseline-read1-16k.log`.
  Download loop is inherited from pinned upstream (our block1024 vs4096).
  Framework's ten 1-second write retries are a hypothesis, not proof; SD early
  read remains possible. Host-only telemetry now includes declared length and
  prefix-match boolean; 18 tests pass. No firmware change or reconnect.

- 2026-09-22 outbound measurement correction is HOST ONLY: benchmark readback
  now uses HTTPResponse.read1 so a sub-4096-byte prefix is counted before a
  later timeout; bounded failure events retain counts/timing/type without
  private content/identity/path. Exact SHA/size gate and original exceptions
  remain. 18 benchmark tests pass (`build/readback-observation-tests.log`).
  No device request or firmware change; old missing prefix counts are unknown.

- 2026-09-22 frozen baseline 16 KiB trial: three stream ACKs, final CRC and
  publication passed; Content-Length HTTP readback timed out, so downloaded
  SHA/end-to-end acceptance failed. Cleanup identity check and deletion passed
  without reconnect. See `build/frozen-baseline-plain-16k.log` and benchmark
  document. Prior statistics failure was chunked; missing chunk termination
  cannot alone explain both. Focus investigation on outbound HTTP delivery;
  do not call this failed stream reception or install another image.

- 2026-09-22 unchanged `w998f2f9d`: a single 1 KiB stream trial WITHOUT the
  failing optional transfer-stats API passed final CRC, publication, downloaded
  SHA256, identity/version/uptime checks and cleanup. No firmware/network/user
  intervention. Evidence: `build/frozen-baseline-plain-1k.log` and
  `docs/TRANSFER_BENCHMARK.md`. This is below the 4 KiB window (no intermediate
  ACK), not large-file/app/X4 acceptance. Diagnostic failure must not block
  or be conflated with ordinary data-path validation.

- 2026-09-22 upstream/SDK exclusion audit: CrossPoint #1160 is a similar slow
  upload report closed as stale, not a verified fix; #3593 is a different X4
  local-file-browser crash. Historical IDF #9059 requires modem sleep/MAC-BB
  power-down conditions not established here. Our server already calls
  WiFi.setSleep(false); its return is unchecked, so do not claim measured
  runtime mode or add the same fix again. Links/effective-source locations are
  in `docs/UPSTREAM_CONNECTIVITY_COMPARISON.md`. No deployment/settings change.

- 2026-09-22 passive-association correction, LOCAL ONLY: removed the gateway
  TCP probe, blocking select and probe-triggered WiFi.reconnect from the File
  Transfer loop. The driver retains normal auto-reconnect; actual association
  loss retains the >5-minute abandon grace. Pure AssociationPolicy tests cover
  connected idle, loss starting at uptime0, recovery/reset, boundary and wrap.
  188 host checks, strict cppcheck and default build pass (logs
  `build/passive-association-*.log`). No app wire change or new heap allocation.
  This removes an unjustified intervention, not a verified hardware root cause.
  Nothing was uploaded; the installed `w998f2f9d` still contains the old ladder.

- 2026-09-22 upload loop stopped per user objection. Read-only source audit
  found a fork-only confounder: gateway TCP timeout can trigger WiFi.reconnect
  despite WL_CONNECTED (10-second probes, 1-second timeout, three strikes).
  Pinned upstream has passive association tracking instead. Mac gateway TCP
  checks were 3/3 successful; actual reader-side branch execution is unknown.
  Optional WS allocation is threshold-dependent (current threshold16KiB), so
  freeing memory can enable another owner. See updated upstream comparison.
  These are source-confirmed mechanisms, not a proven physical root cause;
  no executable changes, rebuild or deployment were made during this audit.

- 2026-09-22 bootstrap installation confirmed: same X3 `/api/status` reports
  exact `1.6.6-dev-main-b8e38e39-sta-recovery-w998f2f9d`, uptime84, heap12824.
  Subsequent control requests were intermittent/failed before any benchmark
  payload. One complete transfer-stats response shows attempt0, all counters0;
  HTTP/1.0 is not a proven workaround (later connect also failed). ICMP 0/3.
  Details in `docs/TRANSFER_BENCHMARK.md`. Do not ask for another mode switch or
  SD swap as a routine next step; keep the user's shared-network setup intact.

- 2026-09-22 SD bootstrap staged: verified `w998f2f9d` was copied as root
  `update.bin` on the user's reader SD, reread SHA256 matched
  `2fb59ea48580bce5381f276522da37f97bca054341314e58e5792db680250c23`, and
  the disk was cleanly ejected. Previous update image remains at
  `firmware-backups/before-w998f2f9d-20260922.bin` on that card. Books, settings
  and fonts were not changed. Installation still requires device confirmation.
  User explicitly requests minimizing card swaps and forbids speculative
  connection-mode churn: keep the existing shared-network STA workflow and
  use the counters before changing transport/radio settings again.

- 2026-09-22 production regression: current `gh_release` builds successfully;
  image is 6033168 bytes, version 1.6.6, SHA256
  `99a606cd04ec8fe64a0d2f214bdb6c6c9f6fb2a5970b68d8becb7573f489d3be`.
  Binary strings contain neither developer flash/transfer-stats routes nor
  transfer-counter markers. The shipping Swift validator accepts this actual
  artifact. 187 host checks and 20 uploader + 16 benchmark tests also pass.
  This is a local product-build check, not hardware/release sign-off. Build
  staging now points to gh_release; use preserved `build/bootstrap/w998f2f9d.bin`
  for the pending instrumented bootstrap, not the mutable `firmware/update.bin`.

- 2026-09-22 bootstrap image ready, not installed: `sta_recovery` build
  `1.6.6-dev-main-b8e38e39-sta-recovery-w998f2f9d`, 6036272 bytes, SHA256
  `2fb59ea48580bce5381f276522da37f97bca054341314e58e5792db680250c23`.
  It includes transfer counters, metric/renderer/state fixes and the optional
  WS queue lifetime change. Build log `build/bootstrap-current-sta.log`;
  preserved outside the five-archive pruning cycle at ignored
  `build/bootstrap/w998f2f9d.bin`. The sibling app's actual Swift image validator
  accepts it with the exact expected version; wrong-version and missing-file
  checks reject. No SD volume is mounted and no installation is claimed.

- 2026-09-22 optional-provider resource lifetime: `OutboundQueue` replaces the
  always-resident 1200-byte WS command array. Its control object is 16 bytes in
  the ESP ELF; storage is allocated once on the first command of a connected
  session and released on disconnect/replacement. The existing mutex serializes
  enqueue/pop and lifetime transitions. Offline/oversized commands are rejected,
  not truncated or replayed from the queue after reconnect. This is separate
  from durable Pocket choices/card state, which are untouched. 187 host checks,
  default build and strict cppcheck pass (`build/outbound-lifetime-*-final.log`
  and `build/outbound-lifetime-host.log`). Runtime heap/radio improvement and
  physical provider reconnection are unverified; no new firmware installed.

- 2026-09-22 delivery failure confirmed: instrumented `w11d630ad` upload
  terminated after three attempts; last sampled SD ACK was 788380/6034384.
  Post-termination status confirms the same reader remains on `w67fff974`.
  No successful commit/flash or device-counter collection. See
  `docs/TRANSFER_BENCHMARK.md`; do not treat earlier in-progress notes as live.

- 2026-09-22 metric domains: parser and Swift encoder reject cover height
  outside 1..2048 and popup offset ratio outside 0..1, retaining signed zero.
  This prevents a zero metric divisor and out-of-domain float-to-coordinate
  conversion; it is not validation of all fields/composed geometry. Binary
  layout unchanged; out-of-domain older packs are now rejected, not clamped.
  184 host checks, default build, strict cppcheck, 10 Swift pack tests and both
  app platform builds pass. Not installed or physically render-verified.

- 2026-09-22 renderer metric binding: all four theme implementation files now
  read active UITheme metrics instead of compiled defaults (which remain the
  native baseline in theme selection). This repairs paths where pack-aware
  layout was paired with unchanged drawing. Pagination helpers keep divisors
  and item counts positive and safely add row/gap values. 182 CTest checks
  (181 C++ cases plus source-boundary guard), default build and strict cppcheck
  pass. Source guard is not pixel parity; font-derived theme dimensions and
  complete arbitrary-metric range validation remain open. Not included in the
  older `w11d630ad` image currently being transferred; no hardware claim.

- 2026-09-22 benchmark reader-counter collection: optional `--reader-stats`
  validates a <=2048-byte whitelisted record against the pre-upload attempt,
  expected size and terminal outcome, with identity/version/uptime checks.
  Reads happen before and after the upload socket lifetime, never during payload;
  failed observations retain the original upload error and do not count toward
  upload duration. All benchmark paths recheck identity/version before commit.
  16 benchmark + 20 uploader tests pass; physical endpoint collection remains
  pending successful installation of the instrumented developer image.

- 2026-09-22 pack value parity: reader rejects CRC-valid noncanonical bools and
  Float32 infinity/NaN, matching existing Swift encoder rules. Finite layout
  range safety is not established by this check. 179 host tests, default build,
  strict native cppcheck and 8 Swift pack tests pass. This change is newer than
  the `w11d630ad` artifact being delivered; do not claim it is installed.

- Passive TCP observation during `w11d630ad` delivery to old `w67fff974` showed
  a five-second interval with +17232 outgoing and +17232 retransmitted bytes,
  no incoming increment. See `docs/TRANSFER_BENCHMARK.md`; this narrows the
  stall to an observed retransmission interval, not its underlying cause.

- 2026-09-22 bounded transfer instrumentation: developer-only
  `/api/pocket/v1/dev/transfer-stats` retains a <=96-byte last-attempt record,
  with receive/service gaps, SD write/close and reply call timings, and sampled
  heap/block minima. No periodic logging or polling; read after receiving ends.
  Schema/limitations are in `docs/TRANSFER_BENCHMARK.md`. 177 host tests,
  default + sta_recovery + gh_release builds and strict native cppcheck pass.
  Product binary contains neither transfer-stats/flash routes nor metric JSON
  markers (`build/transfer-metrics-release.log`). Built developer artifact
  `pocket-daily-1.6.6-sta_recovery-b8e38e39-20260922-003201.bin`, 6034384 bytes,
  version `1.6.6-dev-main-b8e38e39-sta-recovery-w11d630ad`, SHA256
  `e5b0c93fa0db4c03ec52dc7e41e20895e84642d21c29e582a81111a77341ed5c`.
  Build validation is not installed-device validation; deployment status must
  be checked separately before relying on the endpoint.

- 2026-09-22 transport follow-up: same installed `w67fff974` X3, no flash.
  One 256 KiB stream timed out; another reached CRC-matching OK/publication in
  58.490 s but failed download verification (published benchmark file cleaned).
  Four ACK waits consumed 50.513 s; HTTP 256 KiB upload then broke its pipe before
  commit. Neither transport passed acceptance. `docs/TRANSFER_BENCHMARK.md`
  records evidence and remaining hidden-staging uncertainty. Local progress
  observations now preserve submitted vs ACK/OK offsets without reader polling;
  20 uploader and 8 benchmark tests pass. Next: bounded device-side scheduling,
  receive and SD timing counters, not speculative timeout/pacing increases.

- 2026-09-22 bounded UI-pack loading: `validatePackSource` shares the memory
  parser's grammar using <=128-byte reads, a CRC/digest payload pass and a
  record-header pass. `loadPackFromSd` now uses this directly with incremental
  mbedtls SHA instead of malloc(file size). The input must remain stable during
  validation; existing 8 KiB limit, optional SHA and font rejection are unchanged.
  174 host tests pass, including bounded reads, exact digest coverage, phase read
  failures and truncated files. Default build and strict native cppcheck pass
  (`build/stream-pack-final.log`, `build/stream-pack-final-check.log`). No hardware
  installation or measured runtime heap/throughput claim. Multi-file activation,
  retention, product auth and physical failure tests remain open.

- 2026-09-22 pack activation repair: selection now uses alternating 64-byte
  CRC/generation slots with checked write/flush/readback, explicit revert records,
  legacy text migration and boot fallback to the previous valid pack/version.
  Actual runtime activation owns the status metadata instead of rereading an
  unvalidated selection. Swift publishes unique 23-character revision basenames.
  Names are strict ASCII basenames <=24 bytes; versions <=16 with dots allowed.
  UITheme adopts the preloaded malloc buffer under RenderLock after persistence,
  avoiding a fallible second allocation; best-effort shrink precedes activation.
  Composition/revert start from native metrics, fixing inherited old overrides.
  171 host tests, default build, strict cppcheck (current config + local native
  tool override), Swift pack tests and iOS/macOS builds pass. Pure codec tests
  cover truncation/CRC/tombstone/slot selection and composition, not actual SD
  failure injection or power cuts. Not yet installed on hardware. Streaming
  validation, multi-file transactions, retention and product auth remain open.

- 2026-09-21 UI-pack parity: `test/live_studio/UiPackTest.cpp` now parses and
  applies the same complete mixed int/bool/float fixture pinned by the Swift
  encoder test. This caught the companion's old all-int record encoding;
  the app now emits the registry type and confirms exact pack/version on the
  same reader after apply/revert. Firmware runtime wire behavior did not change.
  167 host tests, default build and strict cppcheck pass. Real app/device UI
  application and transactional activation are not signed off by these tests.

- 2026-09-21 P0/P1 implementation: `scripts/benchmark_transfer.py` compares
  existing HTTP and port-82 upload with verified commit, downloaded SHA and
  identity-checked cleanup of generated inert files. `pocket_put.upload_once`
  now returns sender/credit/final-reply timing without changing the wire.
  17 uploader + 8 benchmark tests pass. Both 1 KiB paths passed on `w67fff974`,
  but HTTP connect and later 256 KiB preflights intermittently timed out.
  No large-file winner or root cause established; see `docs/TRANSFER_BENCHMARK.md`.

- 2026-09-21 implementation follow-up: File Transfer now unloads the selected
  SD font under RenderLock before radio startup; independent Calibre entry
  does the same. Provider OTA chunk decode and pulled-image hashing reuse
  flasher scratch synchronously, removing their separate permanent 4096-byte
  buffer without a new heap allocation or protocol change. Default static RAM
  decreased from 135160 to 131064 B; sta_recovery is 131048 B. Both builds,
  166 host tests, 17 Python tests and strict cppcheck for both environments
  pass. The distinct `w67fff974` build subsequently passed full wireless upload
  (6030416 B, CRC 4FFA1BE2), developer flash, same-reader exact-version check and
  automatic saved-Wi-Fi return without another menu action. Post-install 64 KiB
  reception also passed CRC. Throughput remains about 2.2–2.4 KiB/s in these
  samples; heap varied from 9116 to 10976 B, initially blocking diagnostics.
  Later retrieval identified the saved panic as an older `wec1d3ece` build;
  the new image stayed up through the 337-second post-upload status check.
  CLI/X3 success is not app/X4 or release sign-off. See
  `docs/CORE_CONNECTIVITY_REPAIR.md` for evidence and limitations.

- 2026-09-21 upstream comparison: see `docs/UPSTREAM_CONNECTIVITY_COMPARISON.md`.
  Pocket's version label is independent of upstream release ancestry. Official
  C3 SDK tuning cannot be credited with a blanket 32–37 KB future gain here:
  installed 55.03.311 already disables Wi-Fi TX/RX IRAM optimization. Confirmed
  timer stack configuration delta is 5632 B; the extra 8192 B main stack has a
  documented overflow rationale and must not be blindly reduced. ELF confirms
  19516 B across shared dashboard state, provider OTA decode and outgoing queue;
  offline cards use the shared state too. STA font unload occurs after Wi-Fi
  activation, unlike upstream pre-radio cache release. No new physical A/B or
  code change was performed in this analysis; one status connection failed.

- 2026-09-21 physical recovery baseline: the user installed the SD-staged
  `1.6.6-dev-main-b8e38e39-sta-recovery-wf3e32596` experimental image; live STA
  status confirmed it. A 1 KiB probe and full 6,030,352-byte wireless upload
  succeeded (CRC AE2A4752 plus validated commit). Intentional sender interruption
  resumed at 1,187,840 bytes. `--flow-delay-ms 10` paced the remaining segment
  at 512-byte granularity within 4 KiB SD windows, averaging 10.7 KiB/s. The
  reader remained responsive without reboot. Image SHA and evidence live in
  `docs/CORE_CONNECTIVITY_REPAIR.md`. This is CLI/X3 staging acceptance, not
  X4, default-driver comparison, app-device or remote-flash/rejoin sign-off.

- 2026-09-21 upload receive follow-up: port 82 now uses direct nonblocking
  recv into existing buffers, avoiding the installed Arduino client's lazy
  1436-byte RX allocation. Direct EOF detection replaces connected()'s
  zero-byte-peek ambiguity; queued bytes are drained before retaining the
  prefix. Socket-pair regression tests exercise this boundary. Physical 1 KiB
  transfer on the old audit image reached RESUME 0 but did not return OK;
  the later SD bootstrap installed both changes (see the physical baseline in
  `docs/CORE_CONNECTIVITY_REPAIR.md`).

- 2026-09-21 core follow-up: remote flash no longer enables the 4 KiB log
  response buffer/trace diagnostics. Explicit `network_diagnostics` restores
  them; default static RAM decreased by 4496 B in build output. Experimental
  `sta_recovery` wraps the installed Arduino `esp_wifi_init` boundary to bound
  dynamic RX/TX to 8 and RX block-ACK to 4; standard/release retain SDK policy.
  Driver configuration is runtime-adjustable despite precompiled libraries.
  See `docs/CORE_CONNECTIVITY_REPAIR.md`; hardware validation is still pending.

- 2026-09-21 working tree: `pocket_put.py` now bounds automatic recovery to
  three attempts and checks device identity before resuming. Protocol/CRC/SD
  failures are terminal and ambiguous commits are not retried. Explicit
  `--dev-flash` extracts the embedded developer version, uses the existing
  developer-only endpoint once and verifies identity + exact distinct version
  after reboot (`--expected-version` is an optional assertion). The
  consumed developer boot marker now starts saved-network association without
  mode selection; normal production installation remains reader-confirmed.
  Verification: 15 Python tests, 162 host tests, default/gh_release builds and
  strict cppcheck passed. The cppcheck package mirror failed; analysis used a
  fresh copy of the current project configuration with the installed native
  cppcheck package substituted, without changing build flags or suppressions.
  Hardware bootstrap from the installed audit image remains unverified.

- Firmware: this repository (`https://github.com/puritysb/pocket-daily-firmware`),
  checked out on this host at `/Users/puritysb/git/pocket-daily-firmware`
- App Store app: sibling directory `pocket-daily` next to this clone
  (`https://github.com/puritysb/pocket-daily`), on this host at
  `/Users/puritysb/git/pocket-daily`

The host checkout root moved from `~/github/` to `~/git/` (noted 2026-09-19).
Absolute `~/github/` paths in either repository's older notes are stale;
resolve the sibling repository relative to this checkout.

The firmware repository owns device behavior, endpoints, local persistence,
memory gates, and flashing. The app repository owns the Apple-platform client.
Nearby Sync v1 is a contract between them; do not change only one side.

The old combined repository and its generated memories used the path
`crosspoint-agentdeck` and mixed firmware, AgentDeck, and app concerns. Do not
copy those memories wholesale. Promote only a verified, firmware-relevant fact.

## Git topology

- `origin`: `puritysb/pocket-daily-firmware`
- Product branch: `main`
- `upstream`: `crosspoint-reader/crosspoint-reader`
- Upstream foundation branch: `upstream/master`

Pocket Daily product directories are intentionally downstream-only. Use the
repository sync script and merge upstream; never rebase the product history.

## Durable release constraints

- 2026-09-21 working-tree contract: optional `uploadStreamWindow:4096` / Window
  header / ACK replies bound companion payload to one SD-accepted block at a
  time (existing staging buffer, no added payload allocation). Upload focus
  lasts until socket cleanup, then waits five seconds before restoring WS.
  Stream service precedes HTTP; radio probes pause during useful traffic and
  distinguish refused ports from timeouts and local socket failures. Completed
  staging prefixes can resume after a lost OK; all resume state is still
  RAM-only. The companion implementation is in sibling `pocket-daily`.
  Pocket Sync → Join a Network uses an app-only COMPANION profile (preferences
  plus packs, no browser routes/mDNS/WebDAV/UDP); File Transfer stays compatible.
  Missing upload directories are created at staging, and short UI-pack files
  are rejected before reading their SHA header fields.

- X3 and X4 are no-PSRAM ESP32-C3 devices.
- `docs/nearby-sync-v1.md` defines mandatory runtime heap and responsiveness
  gates for both models.
- A host build and host unit tests are necessary but do not satisfy hardware
  sign-off.
- `firmware/LATEST_BUILD.txt` identifies the locally staged ignored artifact.
- GitHub releases are the production OTA source; `firmware.bin` must be present
  as an exact release asset name.
- App Store timing is independent, but Nearby Sync v1 must be frozen and
  compatible before either side is presented as production-ready.

## Exact companion preview contract — 2026-09-02

- `silentRestartToPocketNearbySync()` writes the current framebuffer row by row
  to `/.crosspoint/pocket-screen-preview.bmp` before drawing the loading popup.
  The path is transient and is removed when Nearby Sync returns to Pocket Daily.
- Only the low-memory `POCKET_SYNC` web profile advertises and serves that file:
  `/api/status` reports `screenPreviewAvailable`/`screenPreviewBytes`, and
  `/api/pocket/v1/screen-preview` streams the BMP in bounded chunks (4 KiB
  transient batch, 1 KiB stack fallback).
  Ordinary File Transfer does not expose the cached screen.
- The implementation adds no framebuffer-sized heap allocation. The 2026-09-02
  default build, strict cppcheck, and all 124 host tests passed; physical X3
  framebuffer capture and local HTTP transfer still require device sign-off.

## Resumable batched upload stream — 2026-09-02

- `src/pocket_daily/upload_stream_protocol.{h,cpp}` owns the `POCKET-PUT/1`
  grammar (header parsing, staging-path rules, CRC32, reply formatting) without
  Arduino types; `test/pocket_daily_upload_stream` covers it on the host.
- The port-82 listener batches socket bytes into a transient 4 KiB buffer
  (static 768-byte fallback), flushes sector-aligned multi-block writes,
  suspends the loop watchdog once per flush, and preallocates the staging file
  via the new `HalFile::preAllocate` wrapper when a contiguous span exists.
- Transport failures during payload retain the staging file plus an in-RAM
  verified prefix; `Resume: 1` is answered with `RESUME <received>` and
  `/api/status` advertises `uploadStreamResume`. Stale `.pocket-*.part` files
  in the destination directory are swept when a new transfer starts.
- Verified on 2026-09-02: default build, strict cppcheck, and 131 host tests.
  Throughput, watchdog margin, and resume after a real hotspot drop still need
  X3 and X4 hardware sign-off; no measured KB/s figure is recorded yet.

## Hardware test path and private-AP heap — 2026-09-03

- Hardware iteration goes through File Transfer → Join a Network and
  `scripts/pocket_put.py` (push over port 82, verified commit, verbatim reader
  replies). The user explicitly prefers this over SD-card swapping. The X3
  STA profile has no mDNS; find the reader by probing `/api/status` across the
  LAN.
- Measured 2026-09-03 on an X3 running the 2026-09-02 01:30 build: private
  Nearby Sync AP left 6.4-7.0 KB free heap; STA File Transfer left 16.3 KB. The
  retained crash report was a task-watchdog reset with breadcrumb
  `nearby:screen-preview`, and the reader hung twice more within seconds of
  the companion's post-connect preview/crash fetches. Baseline stream
  throughput on that build over STA: 6,012,768 bytes in 57.5 s (~105 KB/s).
- Mitigations in the tree: the upload stream and diagnostic handlers borrow
  the flasher's idle 4 KiB static staging buffer instead of allocating;
  `/api/status` reports `diagnosticsAffordable` and hides preview/crash
  availability below 10 KB free heap; those handlers answer 503 there.
  Physical confirmation of the new build is still pending.

## Transfer path decision — 2026-09-05

- The private Nearby Sync hotspot is the structurally weakest transport on the
  no-PSRAM X3: softAP+DHCP consume ~53 KB, leaving ~20 KB after the 2026-09
  memory work (was ~7 KB), and it is sensitive to weak signal. Every crash and
  disconnect observed in this effort occurred only on that path.
- File Transfer → Join a Network (STA) is the reliable primary path for
  firmware OTA staging and content transfer: dozens of transfers completed,
  verified, and resumed without a single failure at 100-240 KB/s. The
  companion app discovers the reader on the LAN automatically and uses the same
  port-82 stream, so no protocol change was needed.
- Decision: treat Join a Network as the default OTA/content path in guidance
  and app messaging; keep the hotspot as the fallback for readers without a
  shared Wi-Fi network. The SD startup trail (`/nearby_ap_log.txt`) proved the
  hotspot web server now starts cleanly (20.5 KB free, watchdog armed).

## Resume on another machine — 2026-09-05

Clone both repositories as siblings (`pocket-daily-firmware` next to
`pocket-daily`); the app guide and skills reference the firmware by that path.

1. Build: `./scripts/pio.sh run -e default` (stages `firmware/update.bin`,
   ignored; read `firmware/LATEST_BUILD.txt`). Host tests:
   `cmake -S test -B build/host-tests && cmake --build build/host-tests &&
   ctest --test-dir build/host-tests`.
2. Deploy for iteration: reader → File Transfer → Join a Network, find it by
   probing `/api/status` across the LAN (X3 STA has no mDNS), then
   `python3 scripts/pocket_put.py firmware/update.bin --target update.bin --host <ip>`.
   Install on the reader (Settings → System → Update firmware) and confirm the
   `-w<fingerprint>` in `/api/status` matches
   `strings firmware/update.bin | grep 'Starting CrossPoint version'`.
3. Verified on X3 with the committed tree: batched/resumable port-82 stream,
   verified commit, private-AP web server starting with ~20 KB free
   (`/nearby_ap_log.txt` trail), and an app-driven LAN firmware transfer that
   the reader then installed and reported running.
4. Open items: X4 hardware sign-off, the private hotspot path is a fallback
   (weak-signal sensitive), and `platformio.local.ini` is machine-local.

## Reader freeze and abort on a rebuilt partial section — 2026-09-08

Symptom on an X3 running `1.4.1-dev-main-551001f5`: a multi-chapter novel
stopped responding to forward page turns at `~63/125`, and a long-press Confirm
then rebooted the device. The retained crash report showed `Reset reason: panic`
/ `abort() was called`, with the last log lines laying out pages 58-63 followed
by a 4,005 ms tiled grayscale render.

Cause, in the v130 incremental-build path:

- `Section::startBuild` pins `pageCount` to a loaded partial's watermark until
  the rebuild lays out *more* pages than the partial covers. The reader's
  background pump in `EpubReaderActivity::loop()` only ticked while
  `pageCount < currentPage + BUILD_WINDOW_AHEAD`, which is false during that
  catch-up phase, so the rebuild never advanced in the background. Reaching the
  watermark then forced `render()` to re-lay the whole 63-page prefix
  synchronously while holding the RenderLock — seconds of no response to the
  page-turn button.
- That re-parse keeps expat, the CSS parser, the page LUT, and the live
  `ParsedText`/`Page` alive across the reading path's own peak (tiled grayscale
  render + font prewarm). Layout allocated pages and lines with bare `new` /
  `std::make_shared`, which with `-fno-exceptions` calls `abort()` instead of
  returning null. That is the reboot.
- The default `longPressMenuFunction` is `LP_MENU_BILINGUAL_TOGGLE`, so an
  ordinary long press ran `cycleBilingualMode()`. It dropped the section
  whenever the *committed* cache was not tagged mode-agnostic — which a partial
  never was — forcing yet another full re-parse of a monolingual chapter at the
  worst moment.

Fixes (verified: `pio run -e default`, strict `pio check`, 131/131 host tests):

- `Section::isCatchingUp()` / `mayHaveMorePages()`. The background pump keeps
  ticking through the catch-up phase, so the rebuild passes the watermark before
  the reader arrives. `mayHaveMorePages()` reads the partial's watermark trailer
  so a forward turn at a suspended build's last page still moves to the next
  spine instead of stalling.
- Heap floor `BUILD_MIN_FREE_BLOCK` (12 KB largest free block): the reader
  suspends the build — persisting it as a partial and freeing the parser —
  before a render can hit an exhausted heap. Guarded on `pageCount > 0` so a
  suspend never drops the section below the page being read.
- Every page/line/block allocation in `ChapterHtmlSlimParser` is now
  `new (std::nothrow)` with a null check that sets `outOfMemory_`.
  `parseStep()`/`finishParse()` turn that into a hard parse error and
  `Section::finalizeBuild()` abandons rather than committing a cache that
  silently drops text.
- A parse that has not met a bilingual marker is tagged `BILINGUAL_MODE_ANY`
  even when suspended as a partial, and `cycleBilingualMode()` consults the live
  parse via `currentlyModeAgnostic()`. Pages before the first marker are
  layout-identical in every mode and the rebuild re-derives them, so this is
  safe without a `SECTION_FILE_VERSION` bump: the binary layout is unchanged and
  both firmware directions read the field correctly.

Not yet verified on hardware: the reader was not reflashed with this build, so
the freeze and the abort are fixed in code review and host tests only.

## Maintenance

Record only durable decisions, verified baselines, protocol contracts, and
release evidence. Date mutable facts, name their source of truth, and replace
stale notes rather than accumulating contradictions. A memory entry must be
committed together with the change it describes; uncommitted work is not a
verified baseline.

## Device session: old-build verification + 6 MB grind cut short — 2026-09-21

Supersedes the "no hardware verification yet" line in the sprint entry for the
items below; all results are from the X3 (5B09AF70) at 192.168.68.68, File
Transfer STA, on the **lean heapmap build** ("1.6.6-audit", pre-refactor —
dev/render 404 proves no ENABLE_DEV_REMOTE_FLASH, so no dev/flash endpoint).

- **Verified on device (old build):** /api/status bytes captured (518 B,
  pre-refactor baseline — copy in `~/pocket-verify-20260921/` on this host);
  port-82 upload **twice** with resume banking across radio-death windows
  (3-attempt run: window dies, ladder recovers, RESUME continues — the
  banking design works exactly as intended); `/api/pocket/v1/commit` atomic
  publish 200 with size+crc echo; /mkdir 200; **M3 pack demo complete** —
  hand-built .uipack (3 overrides) uploaded, committed, applied
  (`{"applied":true,...,"overrides":3}`), `activePack` reflected in
  /api/status, reverted (`{"applied":false}`). LS-2 capture confirmed by
  screen-live answering 200 (frame exists) — the 4 KB body send was blocked
  by the radio, not the code.
- **HN-2 settle evidence (File Transfer):** free 8,232 B, largest contiguous
  **3,316 B**, 32-bit-pool minEver 1,184 B. Stack-report on the lean build
  rendered only 2 tasks (caller 8,404; Tmr Svc 3,836) and took 4.1 s.
  Live evidence for the mode-lifecycle note: `liveStudio.mode:"poll"` — the
  WS listener's 40 KB gate can never open at the 8 KB settle heap.
- **Radio pattern reconfirmed:** back-to-back requests kill the window;
  blind fast retries prolong the dead phase. The probe-gated sniper client
  (`put82big.py`) is the right shape. 6 MB push banked 157,292 B in ~9 min
  of grinding (10-130 KB per window) — projected hours; cut short by choice,
  not by loss (bank never regressed).
- **Delivery-path discovery:** Settings hosts the SD firmware picker
  (`SdFirmwareUpdateActivity`, .bin browser, validates at confirmation with
  the reader's own Confirm) — a Wi-Fi-delivered /update.bin needs no UP+POWER
  and no card removal. /update.bin has no other consumer on the lean build.
- **Ready for next session:** post-refactor `heapmap` build staged at
  `~/Desktop/update.bin` (6,072,720 B, same "1.6.6-audit" version string for
  a faithful byte-compare; sha256 66c1612a…). Test pack remains on-device at
  `/pocket-daily/ui-packs/host-verify.uipack` for the new `listPacks()`.
  Next-session checklist: flash (SD bridge or finish the 6 MB push on the new
  build), byte-compare with `compare_status.py`, route 404 matrix, listPacks
  shows host-verify, apply/revert, port-82 + commit on the refactored path,
  WS probe (only if boot-time heap opens the listener), then the overnight
  6 MB stress on the *refactored* stream and the OTA self-update test.

## Layer re-separation sprint complete — 2026-09-20

- The seam sprint (S1–S9, commits 227d45e7…e0bbd721) is done: the Pocket web
  stack now lives under `src/pocket_daily/web/` + `src/pocket_daily/boot/`
  (`Profile`, `UploadStreamServer`, `PocketStatus`, `LiveStudioService`,
  `PocketEndpoints`, `Host` bundles, `PrivateApPolicy`, `StaRadioWatch`,
  `RadioHealthPolicy`, `ProductBoot`), and inherited files keep only hooks.
  Residual Pocket footprint: `CrossPointWebServer.cpp` ~150 lines (from
  +1,696), header ~40, `main.cpp` ~30, the Activity ~180 (NearbySync drive
  stays by design).
- `docs/SEAM.md` is now the source of truth for the boundary: hook inventory,
  E1–E5 exceptions, the carried-patch register (EpubReaderActivity,
  HttpDownloader, lib/Epub — upstream-PR candidates; never send the product
  stack upstream), include rules, the five mirror obligations (status base
  fields, silent-reboot magic 0xC1EAB007, reboot-target numbering,
  hidden-file rule, StaRadioWatch ladder), and the merge routine. Any new
  Pocket touchpoint in an inherited file must be registered there in the same
  change.
- Verified host-side only: both pio envs (`default`, `gh_release`) build, 157
  host tests pass, strict cppcheck clean. S8 (`UiPackStore::listPacks`) is an
  isolated commit because its allocation profile changed (no per-entry String
  churn). **No hardware verification yet** — the device-session bundle is
  still pending: /api/status byte-compare pre/post refactor, per-profile route
  404 matrix, port-82 upload+commit AND chunked /upload+commit, WS
  subscribe/hello/status/frame + pack-apply repaint + transfer focus,
  private-AP stream with watchdog, then the older pending items (6 MB stress,
  M3 pack demo, OTA self-update). Sniper pattern; do not claim device
  behavior from host results.
- Next: `docs/mode-lifecycle-resources.md` (same session) holds the
  mode-lifecycle design — reboot-as-boundary model, resource inventory, and
  the evidence-gated increment list. Implementation next session with HN-2
  heap-map evidence.

## Live studio direction — 2026-09-19

- Agreed direction with the companion app: a live studio over the reader —
  WS event push on STA (>= 40 KB free heap), throttled `screen-live` frame
  capture under `RenderLock` (pure `LiveFramePolicy`, >= 10 KB heap gate),
  and a validated `.uipack` format (ThemeMetrics field-id whitelist,
  string/font overrides, hash-verified, atomic install, apply via
  `UITheme::reload()` metrics-source swap). Reading path stays native.
- Design docs committed, nothing implemented yet:
  `docs/live-studio-v1.md` (this repo, the contract) and
  `docs/LIVE_STUDIO_DESIGN.md` in the sibling app repo. Firmware phases
  LS-1 (WS + events) → LS-2 (screen-live) → LS-3 (pack loader) → LS-4
  (host renderer C ABI + golden parity). Flash contingency: builtin fonts
  (2.43 MB, `OMIT_FONTS` hook exists) move to SD if the engine needs room.
- The host renderer is also built here and consumed by the app as
  `libpdui_host.a` with provenance — an approved exception to the
  no-binaries-cross boundary rule for this one artifact.

## v1.6.6 recovery install + post-install reality — 2026-09-20

- v1.6.6 (a2d11e10, platform 55.03.311, diet phase 1, HN-1, 12 KiB listener
  gate) was released via the tag workflow and installed with the one-time
  SD recovery bridge (UP+POWER), which the user approved after every
  wireless delivery path on the old build was measured dead.
- Post-install on X3: boots clean, advertises LIVE (push/frames/uiPacks),
  File Transfer heap baseline ~8.1 KiB (diet did NOT reach the morning's
  13 KiB; the 3.3.11 core is larger). The old build's OTA "Update Failed"
  was NOT the cert bundle (Sectigo E46 root present in both old and new
  builds) - likely TLS handshake heap; v1.6.6's own OTA is untested until
  the next release.
- RADIO: materially better but not fixed. 6 MB pushes still break (~131 KiB
  first window), BUT the radio now returns windows ON ITS OWN (13+ windows
  in 15 min with no user re-entry - HN-1 ladder + platform effect), and
  resume-banking works across windows (328 KiB banked before the daemon
  restart). Long dead phases remain where the ladder (up to
  esp_wifi_stop/start) exhausts without recovery - the ladder needs a
  final escalation (e.g. guided reboot into File Transfer) or the heap
  must rise enough that death stops happening.
- OPEN (HN-2 phase 2 - the remaining root cause): File Transfer consumes
  ~370 of 380 KiB. Next: dev-build heap breakdown endpoint
  (heap_caps per-task info), then the real diet; upstream fragmentation
  fixes (#3521, #3518) are port candidates. The stress-transfer completion
  and the M3 pack demo (needs one radio window) remain to be exercised.

## Evening radio degradation curve — 2026-09-19

- Same firmware build that moved 6 MB repeatedly in the morning degraded
  monotonically through the evening: mode re-entry recovery window shrank
  from minutes to one request, then cold boots gave only 1-2 requests
  before the radio went deaf (ARP unanswered). Heap at cold boot also
  settled ~9.8-10 KB on LS-3 builds vs 13-16 KB on LS-2 builds - a real
  LS-3 heap regression (pack override buffers + boot fragmentation),
  partially addressed by the shared scratch buffer in `fbadf188`, but the
  radio death is environmental: firmware state resets on cold boot while
  the death returned faster each time.
- Most likely: router/AP-side state or evening 2.4 GHz congestion. Next
  device session: reboot the ROUTER first, cold-boot the reader, then push
  + dev/flash `fbadf188` and run the pack demo. The LS-3 heap baseline
  (~10 KB) still needs a diet - audit UITheme pack buffers and boot-time
  malloc/free fragmentation if the listener gate keeps poll-only.

## M2/M3/LS-3 complete in code — 2026-09-19 late session

- LS-3 shipped: generated field registry (63 fields), .uipack container
  validation (host-tested), SD store with optional SHA-256, boot apply,
  list/apply endpoints, liveStudio advertisement with active pack. App side
  M2 (LiveSyncClient + frame canvas) and M3 (UiPackEncoder + ThemePackInspector
  with Apply live / Revert) landed in pocket-daily (`9e0e4ef`, `34b5dc4`).
- A layout bug shipped in the first pass and was caught by the APP encoder
  crashing: counts/payloadLen/crc/sha sat at 64/72/76/80 in a 100-byte
  header, overlapping minFirmware and truncating SHA. Fixed to the
  documented 120-byte layout everywhere (`d03558de`); host tests and the
  Python builder updated; app tests 65/65.
- DEVICE SIGN-OFF PENDING for LS-3: the offsets-fix build (`d03558de`) is
  NOT yet flashed - the staged /update.bin on the reader holds the
  pre-fix LS-3 worktree build (offsets wrong; do not apply packs from the
  app until reflashed). Blocked by the radio: after a mode re-entry the
  reader answered exactly one HTTP request and went deaf again, worse than
  the morning zombie. Environmental suspect (AP/router-side session or
  power-save mismatch); re-verify on a stable link before blaming firmware.
- Next device session: power-cycle, confirm .64, push + dev/flash
  `d03558de`, Confirm once, then the end-to-end pack demo (upload -> apply
  -> frame diff -> revert).

## LS-2 wrap-up — 2026-09-19 (end of session)

- FULLY VERIFIED end-to-end: complete multi-chunk `screen-live` fetch of the
  53,918 B BMP (528x792) on a healthy link; crash report unchanged through
  all incidents (4,245 B - no panics anywhere today).
- The "offline with QR on screen" incidents are a Wi-Fi zombie, not a code
  freeze: power stayed on, keys worked, user re-entered the mode and it
  recovered. `WiFi.setSleep(false)` was already active, so modem sleep is
  not the cause. Trigger pattern: heavy short-lived HTTP connections
  (chunk-per-connection test scripts + status hammering) on a link that
  swung -41..-76 dBm all day. `WL_CONNECTED` stays true while deaf; only a
  Wi-Fi re-init (mode re-entry) recovers it.
- Client discipline recorded in the contract doc: persistent connection per
  frame's chunks, paced fetches, no tight polling. The M2 app must use
  URLSession connection reuse from day one; re-verify with disciplined
  traffic before considering firmware-side lwIP tuning (PCB/MBOX) or an
  active-probe reconnect in the activity's health check.

## LS-2 stall root cause fixed — 2026-09-19

- The ~50 s loop stall was links2004 `broadcastTXT` blocking on a WS peer
  that died without a close handshake, fed repeatedly by the 15 s status
  keepalive: the single activity loop crawled at ~one pass per TCP
  retransmit cycle (diagnosed via the dev trace ring - one heartbeat per
  ~30 s dump) until power cycle. Fix (`c385fc8b`): track the single
  subscribed client, `clientIsConnected` guard before every send with
  subscription teardown on a miss, `sendTXT` instead of broadcast, and no
  periodic keepalive (change-driven pushes only; the app polls live values).
  Verified on X3: frames flow, and after an abrupt client kill (no close
  frame) status/render stayed responsive at 0-0.8 s for 18 s+.
- Dev tooling that cracked it, all `ENABLE_DEV_REMOTE_FLASH`-only:
  `DevTrace` 48-entry RAM ring + `/api/pocket/v1/dev/live-debug` dump. The
  dump currently prints only the newest entry (chunked-sendContent quirk) -
  still enough to spot heartbeat gaps; fix if it is needed again.
- REMAINS OPEN, new failure mode seen after the fix: the reader went fully
  offline (no ping) right after a partial screen-live fetch (first 4 KiB
  chunk OK, then nothing). `WIFI_ABANDON_MS` is 5 min, so a blip should not
  abandon. Cause unknown: sustained Wi-Fi degradation, a panic reboot, or
  power. Next time it happens, READ THE READER SCREEN first - network
  selection menu = Wi-Fi abandon path, Pocket Daily shell = reboot, dark =
  power. Multi-chunk BMP fetch is still unverified end-to-end (single chunk
  + header validated). Today's link was flaky all day (rssi -41..-76,
  broken-pipe storms during pushes).

## LS-2 live frames — 2026-09-19 hardware session

- Protocol path PROVEN on X3 over STA: `dev/render` trigger → render task
  capture (BMP 53,918 B = 792x528 1-bit) → `frame {seq,bytes}` event →
  chunked `screen-live` fetch returns valid BMP. hello/subscribe/status
  push flows all pass. Multi-chunk fetch gate lowered to 6 KiB
  (`LIVE_FETCH_MIN_FREE_HEAP`, commit `a5178a01`) after the 10 KiB
  diagnostics floor starved fetches at ~10.5 KiB idle heap.
- OPEN DEFECT (reproduced twice): a remote render+capture cycle wedges the
  activity loop for ~50 s or longer — render triggers time out, status
  pushes stop, the frame event arrives ~56 s late, fetches stall partial
  (8-12 KiB), and the reader stays unreachable afterwards until power-cycled.
  Suspects: SD write from the render task contending with the display/SPI
  path, a blocked `sendTXT` to a half-dead WS peer inside `handleClient`, or
  transient heap exhaustion. Needs a USB-serial session with breadcrumbs;
  do NOT trust "capture works" for studio use until this is fixed.
- Dev loop additions shipped and exercised: `POST /api/pocket/v1/dev/flash`
  (validate + flash + boot marker), `POST /api/pocket/v1/dev/render`, and
  the File Transfer boot return (commit `1d5b6940`/`469d8d4e`). The flash
  loop was used end-to-end twice; it needs exactly one Confirm after reboot.
- Also open: no WS wake-lock (the reader can sleep mid-session), and the
  deployed dev build's version string lags its commit (`-w<worktree>` from
  pre-commit builds) — build after committing for exact install checks.
- The File Transfer status screen only repaints on Wi-Fi-bar changes;
  arrow keys do nothing there. Use `dev/render` to force a render.

## LS-1 hardware sign-off — 2026-09-19

- X3 (`5B09AF70`) installed `1.4.1-dev-main-df75f78d` and joined STA File
  Transfer: `/api/status` advertised `liveStudio {mode:"push", wsPort:81}`
  with ~14 KB free. A raw WebSocket client verified hello (proto live-studio/1
  + deviceID), ping→pong, subscribe→immediate status snapshot, and repeated
  status pushes; unsubscribe + close were clean. `df75f78d` also lowered the
  listener gate to 12 KB after the draft 40 KB gate never opened (measured
  ~15 KB free in this profile) and suspended pushes during active uploads.
- Post-install boot returns to the Pocket Daily shell with Wi-Fi off —
  intended: the radio is powered only inside network activities, so push
  exists only while the reader sits in a network mode.
- Not exercisable on STA: the `prefs` event. `/api/pocket/v1/preferences` is
  registered for POCKET_SYNC only, so a FILE_TRANSFER client gets 404.
  Extending preferences (or a studio read path) to FILE_TRANSFER is a
  candidate LS-2-adjacent change; the event itself is covered by host tests.

## LS-1 live-studio event push — 2026-09-19

- `src/pocket_daily/live_studio/LiveStudioEvents.{h,cpp}` implements the
  pure event protocol (hello/status/prefs/bye encoders, subscribe/unsubscribe/
  ping parser with interval clamping, `shouldCaptureFrame` policy) and is
  host-tested in `test/live_studio/`.
- `CrossPointWebServer` starts the WebSocket on STA when free heap ≥ 40 KB
  (`liveStudioPush`), sends `hello` on connect, pushes the full status JSON
  on stable-field change (15 s keepalive, 500 ms spacing), broadcasts
  `prefs` after a preferences POST, and `bye` on stop. Legacy binary/START
  upload frames are now FULL-profile only. `/api/status` advertises
  `liveStudio {mode, wsPort, frameStream:false, uiPacks:false}`.
- Verified: default build, strict cppcheck, 147/147 host tests. Physical X3
  push behavior (subscribe + status push while reading) is pending hardware
  sign-off; the app-side consumer landed as M1 in `pocket-daily`.
- Toolchain note: the registry `tool-cppcheck` package for darwin_arm64
  ships an Intel-only binary (`Bad CPU type`). The native 2.11 build now
  persists at ignored `build/native-cppcheck` and
  `pio check -c build/native-check.ini` points there — temp-dir overrides
  get wiped between sessions.
- Format note: the host `bin/clang-format-fix` (v23) wraps long string-literal
  calls and include blocks differently from CI's LLVM-21 clang-format. For
  new files with long literals, expect CI to reject the local output; apply
  CI's diff or update the local pinned formatter before pushing.

## Multi-agent collaboration — 2026-09-19

- OpenCode, Claude Code, and Codex all work in this repository and in the
  sibling app repository. Each repository's `AGENTS.md` is the only project
  instruction file for every agent and this file is the shared cross-agent
  memory. No agent keeps a private instruction file or separate memory.
- 2026-09-24: Claude Code 2.1.277+ reads `AGENTS.md` natively, but only when
  no `CLAUDE.md`/`CLAUDE.local.md` exists in the working directory or above.
  The `CLAUDE.md` → `.skills/SKILL.md` symlink was removed: always-needed
  rules moved into `AGENTS.md`, mechanisms and examples into
  `docs/embedded-reference.md`, and the scoped `lib/Epub/` and
  `src/agentdeck/` guides were renamed to `AGENTS.md`. Skills stay under
  `.claude/skills/` and are indexed from `AGENTS.md` for other agents. Do not
  reintroduce a `CLAUDE.md`.
- Upstream sync hazard: upstream #3058 (2026-08-15, after the 2026-06-29 merge
  base) made its rulebook the real `AGENTS.md`, turned `CLAUDE.md` into a
  symlink to it, and moved its skills from `.claude/skills/` to `.skills/`.
  The next `sync-upstream.sh` merge will conflict on `AGENTS.md` and may
  rename our skills by rename detection. Keep the fork's `AGENTS.md`, port
  upstream rule changes into it or `docs/embedded-reference.md`, keep
  `CLAUDE.md` deleted, and keep skills where Claude Code discovers them.
- Cross-repository protocol work (Nearby Sync v1, upload stream, direct
  sessions) proceeds on both sides in one coordinated effort: verify the
  companion implementation whenever an endpoint, record, or file layout
  changes, and record the paired commits here.
- On 2026-09-19 the previously uncommitted working tree (reader partial-build
  fixes, companion transport sessions, OPDS panic repair) was re-verified
  (default build, strict cppcheck 2.11, 140/140 host tests) and committed as
  `5f1806a3`, `3ddfe641`, and `3d690840`; the app-side counterpart landed in
  `pocket-daily` as direct reader sessions.

## Companion transport sessions — 2026-09-09

- Pocket Sync offers Join a Network (saved reader Wi-Fi) and Nearby Sync
  (existing BLE/private AP). The original STA server profile is preserved.
- `/api/status` adds optional `deviceID` matching BLE and `sessionEnd` for the
  private AP only. POST `/api/pocket/v1/session/end` rejects active transfers,
  responds, then returns to Pocket Daily through the existing low-memory restart.
  Status heartbeats no longer keep an idle AP alive; no automatic flashing occurs.
- The app queues local copies before AP handoff, keys retry UUIDs and installation
  checks by device identity, and defers automatic diagnostics on the private AP.
  Resume state remains RAM-only; reader restart falls back to RESUME 0.
- Default firmware build and 132 host tests passed. Physical iPhone/X3 no-router
  transfer, heap/watchdog, interruption/retry, and install confirmation remain
  pending; see the companion's `docs/CONNECTIVITY_VALIDATION.md`.

- Strict cppcheck passed using a native build of the pinned 2.11 upstream release
  through ignored build/native-check.ini; the registry mirror was unavailable.

## X3 installation verified — 2026-09-09

- After user-side installation, live STA /api/status reported
  `1.4.1-dev-main-fa92806c-wf9376b5a`, matching the staged image exactly, with
  a fresh software restart. New deviceID and sessionEnd=false fields were
  present; port 82 and resume remained available. Direct AP and updated iPhone
  app verification are still pending.

## OPDS catalog allocation panic — 2026-09-11

- User-reported crash report from installed `1.4.1-dev-main-fa92806c-wf9376b5a`
  showed `panic`/`abort`. Symbolization against the preserved matching ELF reached
  `OpdsParser::endElement` vector reallocation/copy while parsing an HTTP feed;
  HTTP-open log showed 9,420 B free / 6,900 B largest block. This report does not
  implicate the user's PDF transfer. Raw report and ELF remain in ignored build/.
- OPDS fetch now streams at most 1 MiB to `/.crosspoint/opds-feed.tmp`, closes
  HTTP and file handles, then parses in 128-byte reads. Scratch is removed on
  completion/failure. Old catalog capacity is released before fetching.
- Parser bounds fields to 2,048 bytes and catalogs to 128 entries, moves entry
  strings, checks contiguous/free heap before growth (4 KiB headroom), and stops
  with an error rather than claiming success with a truncated catalog. Pagination
  capacity is reserved before transferring ownership to the browser.
- Scope is crash repair first; PDF rendering remains unsupported. Host tests use
  bundled Expat with firmware XML flags. Hardware OPDS retry/sign-off is pending.
- Verification: default build, strict cppcheck 2.11 and 140/140 host tests passed.
  Staged `1.4.1-dev-main-fa92806c-w8ea71718` to the X3 over STA: first stream
  interrupted; resume acknowledged offset 1,201,932; final OK/commit 200 matched
  6,023,216 bytes and CRC32 DDC8E685, publishing `/update.bin`. SHA-256:
  `38795190893c922299a92422e3c9170a02827fc5429d2e00fc4558713dbb6635`.
  This proves staging, not installation or a hardware crash fix.
