# Fork Delta Register

Every change this fork carries in files inherited from CrossPoint, classified so
that each one has an owner decision: send it upstream, keep it as a device or
product patch, or delete it. `docs/SEAM.md` owns the *mechanics* of the product
boundary (hooks, include rules, mirror obligations); this register owns the
*intent and evidence* behind each inherited-file change.

Baseline: CrossPoint 1.6.5, `upstream/master` `93e98bb7`, fully merged into
`main` at `04aae8c3`. Audit date 2026-10-01. At that point the fork modified
120 inherited `src/`/`lib/` files (+13.3k/−2.8k lines outside the two product
directories). Reproduce with:

```sh
git diff --name-only --diff-filter=M upstream/master..HEAD -- src lib | grep -v pocket_daily
git diff --numstat upstream/master..HEAD -- src lib | grep -v pocket_daily
```

## Buckets

| Bucket | Meaning | Where it lives | On an upstream merge |
|---|---|---|---|
| **U** upstream candidate | Fixes a defect or cost for any CrossPoint user, independent of Pocket Daily, the companion app, or X3/X4-only constraints | Inherited file, as a separable commit | Keep until accepted upstream, then take upstream's version and delete ours |
| **H** device / fork | Only meaningful under the C3 no-PSRAM X3/X4 budget, or the fork's own formats and release channel | Inherited file, registered here | Reconcile by hand; never send upstream |
| **P** product | Exists for Pocket Daily, the companion contract, BLE, or Live Studio | `src/pocket_daily/` behind SEAM hooks | Re-apply hooks only |
| **D** duplicate | Upstream already has an equivalent | — | Delete ours, keep upstream's |
| **R** fork regression | The fork lost or broke something, usually during a merge | — | Fix in the fork first, with a test |
| **Q** stock deviation | Changes stock behavior a CrossPoint user would notice, without an agreed product reason | — | Decide: promote to P with a reason, or restore stock |

## Evidence standard for a U entry

A candidate is **PR-ready** only when every row below is filled with a checked
fact. Anything missing stays listed under "Missing proof".

1. **Defect / cost:** the concrete failure or measured cost on stock CrossPoint,
   not on the Pocket shell.
2. **Mechanism:** why the change removes it (for example DRAM vs SD I/O count).
3. **Numbers:** before → after, with the command or build that produced them.
   Host numbers must name the corpus. Device numbers must name the board,
   commit, and artifact hash.
4. **No-regression proof:** byte-identical outputs where layout must not change
   (section `.bin`, rendered frames), heap floor and largest block, and the
   existing host suite.
5. **Tests:** host tests that fail before and pass after, runnable on
   `upstream/master` plus the change alone.
6. **Extraction:** the change applies as a standalone commit on
   `upstream/master` with no `pocket_daily` include and no fork-only format.

Status values: `candidate` → `needs-port` (must be rewritten onto current
upstream code) → `evidence-ready` → `pr-open` → `merged-upstream`.

## U — upstream candidates

Ordered by value divided by extraction cost. "Recorded" numbers come from the
cited commit or document and have not been re-measured in this audit.

| ID | Change | Status | Recorded numbers | Existing tests | Missing proof |
|---|---|---|---|---|---|
| U-HAL-1 | `HalFile` write-behind buffer (`HalFile::setWriteBuffer`, `lib/hal/HalStorage.h:139`; used by `Section.cpp:395`) — fd4a58df | candidate | ~59,000 → 83 `write` calls per 100 KB of chapter (fd4a58df, `docs/reader-perf.md`) | `test/hal_storage` `WriteBufferBatchesSmallWritesWithoutChangingBytes` | Device chapter-build time before/after; section `.bin` byte identity on a corpus; peak heap +2 KB. Split from the `ParsedText` half of fd4a58df |
| U-HAL-2 | OOM-safe `HalFile` (`makeUniqueNoThrow<Impl>`, no `assert(impl)`) — 391e12fd, b818e819 | candidate | — (correctness) | `test/hal_storage` StorageAllocation cases | Allocation-failure host test on upstream baseline |
| U-EPUB-1 | Measure each line-break gap once in `ParsedText::computeLineBreaks` — fd4a58df, re-ported 2026-10-01 onto upstream's gap rule | **evidence-ready (host)** | Two whole books (Frankenstein SE + Pocket check book), three layouts, 6,771,584 section bytes: host build 181–204 ms → 150–158 ms (−15 to −17 %), digest identical `0x6b8020d1993c5848` in Release and ASan builds. Earlier record: 5.95 → 3.39 ms per 100 KB with U-HAL-1 (fd4a58df) | `ReadingPositionLayout.SectionFilesMatchTheLayoutGolden` (`test/reader_layout`) | Device chapter-build time on X3/X4; depends on U-EPUB-6 for a deterministic golden |
| U-EPUB-2 | nothrow `Page` allocation plus `outOfMemory_` in `ChapterHtmlSlimParser` — 5f1806a3 | candidate | Field abort at 12,276 B largest block (51191628) | none | Allocation-failure host test proving a clean build failure and no committed partial; hand-split from text-offset hunks |
| U-EPUB-3 | `Section::mayHaveMorePages()` / `isCatchingUp()` so a suspended partial does not skip the rest of the chapter — 5f1806a3 | candidate | — | `test/reader_layout` uses it, no dedicated case | Host test: paused partial, forward turn, assert same spine |
| U-EPUB-4 | Replace U+FFFD titles with the filename, clear U+FFFD authors (`Epub.cpp:17-45`) — 5bdacea0, 819ecd3d | candidate | — | none | Unit test on metadata sanitizing; exclude the `recent.bin` migration |
| U-EPUB-5 | Grapheme-safe hyphenation and CJK breaks (`Hyphenator.cpp:173-195`, `ParsedText.cpp:170-172`) — cd0f98c4 | needs-port | — | `test/hyphenation_eval/HyphenatorClusterTest.cpp`, `test/utf8_compose/Utf8ClusterTest.cpp` | Depends on fork `Utf8.h` helpers; send together with U-FONT-2 |
| U-EPUB-6 | Zero the whole `PageLink::href` buffer (`lib/Epub/Epub/PageLink.h`) | **evidence-ready (host)** | Upstream writes all 128 bytes of `href` (`Page.cpp` serialize) after copying only the string, so section files carry uninitialized heap bytes: the same book produced different section bytes in Release vs ASan (`0xBE` fill) builds until fixed | Golden above is identical across builds only with the fix | Upstream PR: reproduce with ASan `malloc_fill_byte` or two builds; one-line fix |
| U-ZIP-1 | `ZipFile` reads the EOCD signature and fields through `reinterpret_cast<uint32_t*>` at byte offsets (`ZipFile.cpp:253,270`) | candidate (upstream code, unchanged by the fork) | UBSan: misaligned 4-byte loads on every EPUB open (host) | none | `memcpy` reads; UBSan-clean host run. Works on the C3 today, so this is portability/UB, not a device crash |
| U-FONT-1 | Bounded LRU advance cache with batched reads — 347c6ec8 | needs-port | 1,596 distinct syllables: 10,217 → 3 opens, 21,548 → 2 seeks, 21,616 → 419 reads, byte-identical sections (347c6ec8, `PROJECT_MEMORY.md`) | `test/sd_font/SdCardFontAdvanceTest.cpp` | Re-port onto upstream `SdCardFont` without BoundedUI; stock EPUB with >768 distinct CJK codepoints; Latin no-regression |
| U-FONT-2 | Zero-width Default_Ignorable codepoints, emoji clusters drawn once (`lib/Utf8/Utf8.h:121,133`) — 77cec6ab, part of cd0f98c4 | needs-port | — (correctness) | `test/utf8_compose`, `test/gfx_host/GlyphFallbackTest.cpp` | Golden frames for ZWJ/VS16 strings; separate from the PocketSymbols fallback font |
| U-GFX-1 | Ink-row strip culling and `blitGlyph` — d4ae64bb | candidate | Host BW render 0.13 → 0.08 ms, gray strips 0.32 → 0.19 ms, frames identical (`docs/reader-perf.md`) | `GfxHostInkRows.*` in `test/gfx_host` | X4 turn-time A/B; pixel identity in four orientations; caller is fork `ReaderPageRenderer` |
| U-NET-1 | WebDAV GET streams the file (`WebDAVHandler.cpp:365-389`) — 1ed19672 | candidate, **defect inferred** | "only consumed one byte on X3" (commit comment) | none | Reproduce on a stock X4 build: `curl` GET over WebDAV, compare sha256. Upstream calls `client.write(file)` (upstream `WebDAVHandler.cpp:331`), which may bind to `write(uint8_t)` via `operator bool`. Use a ≤1 KiB stack buffer |
| U-NET-2 | `KOReaderSyncClient::errorString` handles `USER_EXISTS` | candidate | — | none | One host assertion |
| U-NET-3 | `HttpDownloader::lastFailure()` stage/code/TLS/heap diagnostics and `OtaFailure` classifier — 1ed19672, c70cc220 | candidate | Verified on X3 2026-09-26 (`PROJECT_MEMORY.md`) | `test/ota_failure` | Strip companion-app wording; stock OTA screen before/after |
| U-SYS-1 | Power-button interrupt latch (`HalGPIO::attachPowerButtonLatch`, `consumePowerHold`) — 6c065027 | candidate | — | none | Host test with fake GPIO/clock (bounce, held press, screenshot combo, X4Pro click window, 10 ms short-press threshold); device loop blocked 2 s still sleeps. Review debounce bypass and ISR IRAM safety first |
| U-SYS-2 | Crash reports for brownout/eFuse/glitch/watchdog, 4 rotated reports, RTC breadcrumb — 4ba46658 | candidate after R-6 | Field task-watchdog report with breadcrumb (`PROJECT_MEMORY.md`) | none | Fix R-6 first; host tests for `isRebootFromCrash`, rotation, begin/clear order |
| U-SYS-3 | `getBaseSettingsList()` built once at boot | candidate | — | none | Heap and largest block during an upload plus settings GET |
| U-UI-1 | Home menu rect height `pageHeight - menuTop - hints` and fixed arrays instead of per-paint vectors (`HomeActivity.cpp:533`) — b3f28f98 | candidate | — | none | Split from the Pocket entry; frame diff |
| U-NET-4 | Release TIME_WAIT PCBs on the server (`src/pocket_daily/web/ServerTimeWait.*`) — 77953e61 | concept only | Eight status reads: 19,016 → 17,468 B without, 19,136–19,176 B with (`PROJECT_MEMORY.md`) | `ServerTimeWaitTest` | Uses lwIP private headers; stock File Transfer polling heap trace |

Larger items that need an RFC with X3/X4 `readerPerf` data rather than a
direct PR:

- **Layout ahead/behind on the render task** (`ReaderLayoutAhead.*`; 0dcab16d,
  a38eca21, bc158e02). X3: unbuilt chapter up to 2,907 ms vs ~4 ms prebuilt;
  backward turns 7.8–9.6 s. Tests in `test/reader_layout`. Depends on
  `PocketDaily::ReaderPerf`, the AA yield and landing-cancel atomics, and
  overlaps upstream's background build and `idlePrewarm`.
- **Deferred progress save (1.5 s idle)** (0dcab16d, 41aea867). Save measured up
  to 419 ms per turn (`docs/reader-perf.md`). Currently inactive, see R-1.
- **XPointer numbering** (`text()[N]` counts non-whitespace runs only, as
  crengine does; 0bdea6f8, 07814f78). 430 golden positions resolve in the app.
  The reference is the app's reimplementation, so a PR needs a golden produced
  by real KOReader against upstream's resolver.

## R — fork regressions

| ID | Regression | Status (2026-10-01) | Test / proof |
|---|---|---|---|
| R-1 | Deferred progress save never ran: upstream's synchronous save in `renderBook()` updated `lastSaved*` before the fork's deferral compared against them | **Fixed**: synchronous block removed; idle `flushPendingProgress` and `onExit` write | `scripts/test_sync_routes.py` `test_page_turns_defer_the_progress_write` (fails on the old code). Device: turn pages, confirm `progress.bin` updates ~1.5 s after the last turn and on exit |
| R-2 | Line-break gap cache lost in the 1.6.5 merge | **Fixed** (U-EPUB-1 re-port) | Layout golden identical; −15 % host layout |
| R-3 | Long-press enum: Bilingual inserted at 3, shifting Dictionary/Reader Menu; stock files misread | **Fixed**: stock values 0–4 restored, Bilingual appended as 5; fork files without the `longPressMenuOrder` marker are remapped (`src/pocket_daily/SettingsMigration.h`); Reader Menu is listed on X3/X4 too because Bilingual follows it; home-key legacy table restored. Default stays Bilingual Toggle (product) | `test/pocket_daily_settings` (stock, pre-fix fork, marked files) |
| R-4 | Wake-refresh inputs dropped; OK-held resume bypassed the boot-loop guard | **Fixed**: Home routes get `needsWakeRefresh`; reader resume (OK held, or waking from the book's Quick Resume page) uses stock's `readerActivityLoadCount` guard and `allowFastInitialReaderRefresh`; unused `rebootedFromPanic` removed | Device: Quick Resume sleep from a book, wake → same page; crash-loop book is not reopened |
| R-5 | Duplicate page-load retry counters | **Fixed**: upstream's `pageLoadRetryCount`/`MAX_PAGE_LOAD_RETRIES` only | Host build |
| R-6 | Crash report lost logs on brownout / no-marker watchdog | **Fixed**: `begin()` keeps diagnostics for every `isRebootFromCrash()` reset; message/stack cleared when no panic hook ran; retained strings terminated | Device only (reset reasons are not host-testable) |
| R-7 | Merge rolled back upstream's `drawHintLabel` wrap and screen-width button positions | **Fixed** (`6a9b8c8c`): themes rebuilt from `upstream/master`; two-line hint labels and panel-width button positions are upstream's again | `git diff upstream/master -- src/components` shows only the CJK font, `StatusBarTitle` and Home paging hunks. Device: hints on X3/X4, all four orientations |
| R-8 | Segmented inflate window no longer on the index path | Open | Confirm the X3 "Failed to index" case on hardware first |
| R-9 | Dead declarations and code | **Fixed**: `goToRecentBooks` removed, `renderBlankSleepScreen` defined again (stock Blank mode), `CssParser` identical to upstream; theme leftovers (`drawList`, `drawTabBar`, `drawKeyboardKey`, `TabInfo`, `MENUDBG`) removed with the theme restoration (`6a9b8c8c`) | `scripts/test_sync_routes.py` `test_stock_themes_carry_no_product_code` |
| R-10 | Two loop-stack macros | **Fixed** structurally (one `#if/#else`); 16 KB kept until `uxTaskGetStackHighWaterMark()` is measured on the companion, BLE and OTA paths | Build |

## Q — stock deviations

Decided 2026-10-01 (owner): Pocket Daily's first button reads **Home**; books
return to the shell that opened them; stock screens host no BLE window; UI
packs and live frame capture leave the firmware and the themes return to stock.

- **Boot and wake routing** (`main.cpp`): Pocket Daily is the default shell
  (`startupApp`); Back held opens Home; OK held or waking from a book's Quick
  Resume page reopens the book with stock's boot-loop guard. Documented in
  `USER_GUIDE.md`.
- **Sleep screen** (`SleepActivity.cpp`): restored to stock for the default page
  (CrossPoint logo), Blank mode and the Home-with-open-book case (no forced
  cover). Kept (product): the WAKE cue setting (`sleepWakeIndicator`) and the
  Pocket Daily Brief sleep frame.
- **Automatic reader font** (`SdCardFontSystem.cpp:573`): PocketSansWorld on the
  SD card overrides the chosen reader family. Open: decide or make it opt-in.
- **Network**: X3 manual File Transfer uses the `FILE_TRANSFER` profile (no
  WebDAV, UDP discovery or WebSocket upload); upload buffer 4 KiB → 1 KiB;
  KOSync TLS heap floor 55,000 B vs upstream 35,000 B; `HttpDownloader` drops
  upstream's `WifiPowerSaveGuard`; OPDS aborts past 128 entries or a 2 KiB field
  where upstream truncates. Open.
- **Stacks**: render task 8 → 12 KB (`ActivityManager.cpp:53`). Open (measure).
- **Themes**: **Done** 2026-10-01. UI packs (`ffe19edf`) and live frame
  capture (`ad38b5db`) are removed and the themes are rebuilt from
  `upstream/master` (`6a9b8c8c`). What remains in `src/components` (+184/−62
  against upstream): per-label `UiCjkFont::fontForText` (see H),
  `StatusBarTitle`, and minimal Home menu paging
  (`BaseTheme::drawMenuPageArrows`) so the Pocket Daily row cannot draw over
  the button hints. Pocket content cards and Articles rows are drawn outside
  the themes (`b5be8778`, `4485e457`).
- **Home**: one Pocket Daily row with the stock `Recent` icon (Classic draws no
  menu icons, as in stock). Stock Home shows 4 rows (+OPDS); with ours, Classic
  fits 5 rows per page on X3/X4, so only an OPDS-configured Home pages (with
  page marks, on Classic and the Lyra themes).
  BLE windows were removed from Home, File Browser and Library.

## D — duplicates to delete

- OPDS field bounding (3d690840): upstream #2475 bounds fields and caps entries.
- `&apos;` entity, KOSync 2xx/204, strongest-AP join (#2655), MAC read (#2960),
  #2526, #2519, #3573, #3581, #3244: already upstream.
- `recent.bin` → JSON migration: upstream removed the binary file (#2464).
- `make_shared` pages (aef25c29): upstream reserves `unique_ptr` elements;
  `Page.cpp` now reserves twice.
- 192-word chunking (51191628): moot with upstream's WordStore;
  `LONG_BLOCK_WORDS`/`WORD_CAPACITY_STEP_LIMIT` are dead.
- ~~`UiCjkFont::fontForText` in themes~~: not a duplicate on X3/X4.
  Upstream's per-string UI fallback (`setupUiFallbacks`/`resolveTextFontId`)
  returns early without PSRAM (`SdCardFontSystem.cpp`), so Korean labels need
  `UiCjkFont`. Reclassified H 2026-10-01.

## H — device and fork patches (kept, never sent upstream)

- Section cache v134, 0xFF partial sentinel, 7/12-byte progress record
  (`lib/Epub/AGENTS.md`, `ReaderProgressCodec.h`); TXT cache v4.
- 12 KB build-suspend floor, X3 76-row render strips, ReaderPerf stages.
- BoundedUI font mode, kerning/ligature lookup hooks, page glyph pinning
  (93017b70, 4dbf8a72), `StatusBarTitle`.
- `UiCjkFont::fontForText` at theme text sites: upstream's
  `SdCardFontSystem::setupUiFallbacks` returns early when there is no PSRAM,
  so without it Korean menu, hint and title labels have no glyphs on X3/X4.
- Verified ESP-IDF TLS only (wolfSSL/SecureNet removed to fit the 6,553,600 B
  OTA slot), static 4 KiB flasher staging buffer, 8 s LAN timeout and 1 KiB RX
  buffer, SD font release before Wi-Fi.
- OTA channel `puritysb/pocket-daily-firmware`, `firmware.bin`, `pocket-v` tags.
- `sdUpdaterAlignmentCompat` shim, X3/X4-only `platformio.ini`, CI changes.

## P — product additions

Documented by their own contracts; the seam mechanics are in `docs/SEAM.md`.

| Feature | Contract | Inherited-file hooks |
|---|---|---|
| Pocket Daily shell, Articles, daily word | `docs/product-architecture.md`, `docs/articles-v1.md` | Home entry, `ActivityManager::goToPocketDaily/goToArticles`, boot routing |
| BLE Nearby Sync and Reading Sync windows | `docs/nearby-sync-v1.md`, `docs/reading-sync-ble-v1.md` | `main.cpp` sleep/wake, `ActivityManager` window arm/close, `allowsExchangeWindow()` |
| Reading-progress offer, exact positions | `docs/reading-progress-v1.md` | `EpubReaderActivity` offer and exit record |
| Bilingual EPUB | `docs/bilingual-epub.md` | Parser filter, reader cycle, long-press value |
| Companion web routes, port-82 upload, private AP | `docs/webserver-endpoints.md`, `docs/transfer-control-v1.md`, `docs/SEAM.md` | Web server hooks E1–E5 |
| Live Studio (WebSocket status/prefs push; UI packs and frame capture removed 2026-10-01) | `docs/live-studio-v1.md` | `wireLiveStudioHost()` thunks in `CrossPointWebServer.cpp`; no theme or render-task hooks |
| Glance, profile, content cards | `docs/pocket-glance-v1.md`, `docs/pocket-profile-v1.md`, `docs/content-*.md` | none outside `src/pocket_daily/` |
| Staged firmware offer, dev remote flash | `docs/dev-update-return.md` | `main.cpp` boot, `SdFirmwareUpdateActivity` |
| Daily Brief sleep frame, WAKE cue | `docs/nearby-sync-v1.md` | `Activity::paintSleepFrame`, `SleepActivity` |

## Maintenance rules

- A change to an inherited file lands with a row here in the same commit.
- A U change is its own commit with no product hunks, so it can be
  cherry-picked onto `upstream/master` for a PR.
- When upstream accepts a U change, the next sync takes upstream's version and
  the row moves to "merged upstream" with the PR number.
- Each upstream sync re-runs the reproduction commands at the top and updates
  the baseline line.
