# Reader page-turn timing

The EPUB reader measures every page turn on the device and keeps the numbers in
one small SD file, so real X3/X4 timings can be read over Wi-Fi without USB.
It records durations, counters and heap figures only: no book text, titles,
paths or positions.

## Collecting timings on a reader

1. Install the firmware and read normally (landscape or portrait, any book).
   Every forward/back turn and the first page after opening a book is recorded.
2. Leave the book (Back to Home, or let the reader sleep). The recorder is also
   written every 32 turns while the reader sits idle on a page.
3. Open **File Transfer → Join a Network** and read the status:
   `curl http://<reader>/api/status` → the `readerPerf` object. The Sync
   heartbeat profiles do not include it (the same gate as `lastBuildError`:
   diagnostics must be affordable, at least 10 KiB free heap).
4. The raw record is `/.crosspoint/reader-perf.bin` (downloadable through File
   Transfer). Deleting it starts a new history; so does a different firmware
   version.

## `/api/status` `readerPerf`

```json
"readerPerf": {
  "format": 1, "version": "1.7.0-dev-...", "turns": 212,
  "fields": "input,section,page,prewarm,bwRender,status,bwRefresh,grayRender,grayRefresh,graySync,save,total,sdOpens,sdReads,sdKB,glyphs,strips,flags,freeHeap,largestBlock",
  "avg": "4,0,9,61,31,6,301,69,118,62,38,661,2,211,16,109",
  "max": "...", "minFreeHeap": 41234, "minLargestBlock": 20480,
  "recent": ["3,0,8,57,30,5,298,66,117,61,40,645,2,205,15,104,14,16,43120,22528", "..."]
}
```

`recent` lists up to 16 turns, newest first; each string follows `fields`.
`avg` and `max` cover every turn recorded by this firmware version (`turns`)
and follow the first 16 names of `fields` (the stages and counters).
All durations are milliseconds.

| Field | Meaning |
|---|---|
| `input` | button handled in `loop()` until `render()` started: the wait for the previous page's render to finish (typically its anti-aliasing tail) or for idle work holding the render lock (a pre-build or background layout tick). Polling latency before the button is seen (up to 50 ms once the CPU idles at 10 MHz after 3 s) is not included. |
| `section` | opening the chapter: cached section file, or laying out pages up to the one shown (first open of a chapter) |
| `page` | reading and decoding the page from the section file |
| `prewarm` | scanning the page's text and loading its glyphs (and the CJK status-bar title's) from the SD font |
| `bwRender` | drawing the page into the BW framebuffer |
| `status` | status bar |
| `bwRefresh` | BW panel update: SPI transfer of the frame plus the waveform busy-wait (FAST, or HALF every *Refresh Frequency* pages) |
| `grayRender` | anti-aliasing: rendering both gray planes strip by strip and streaming them over SPI |
| `grayRefresh` | anti-aliasing waveform busy-wait |
| `graySync` | re-sending the BW frame to both controller RAM planes for the next differential turn |
| `save` | reading-position write. It now happens while idle after the turn and is charged to the turn afterwards; `total` excludes it. |
| `total` | button handled (or render start for the first page) until the page is complete |
| `sdOpens`, `sdReads`, `sdKB` | SD file opens, `HalFile::read` calls and KiB read during the turn |
| `glyphs` | glyphs the page prewarm loaded |
| `strips` | grayscale strip passes (X3: 14) |
| `flags` | bit set: 1 new chapter section, 2 layout ran during the turn, 4 backward turn, 8 HALF refresh, 16 anti-aliasing shown, 32 page has images, 64 chapter adopted from the idle pre-build, 128 first page after opening the book. Without 16, `strips` below 14 means the gray pass was cut because the next turn was already queued. |
| `freeHeap`, `largestBlock` | lowest free heap / largest free block sampled after the prewarm and with the strip scratch allocated (the page render's peak) |

Reading it: `bwRefresh + grayRefresh` is panel physics; `grayRender +
graySync` is mostly SPI (anti-aliasing overhead); `section`, `page`, `prewarm`
and `sdOpens` are SD work the firmware controls. A turn with flag 1 but not 2 or
64 opened a cached chapter; flag 64 means the next chapter was ready before the
turn.

## Record format

`/.crosspoint/reader-perf.bin`, 844 bytes, little-endian, written to a `.tmp`
file and renamed: magic `PDRP`, format 1, stage count 12, counter count 4,
record size 42, ring count, ring next slot, 2 reserved, turns (u32), firmware
version (48 bytes, NUL-terminated), 16 x (sum u32, max u16), min free heap,
min largest block, 16 records (12 x u16 stage ms, 4 x u16 counters, strips u8,
flags u8, free heap u32, largest block u32), CRC-32 over everything before it.
Source: `src/pocket_daily/ReaderPerf.*`; tests: `test/reader_perf`.

The recorder is ~0.9 KB of heap held only while the reader is open
(`ReaderPerf::open/close`); the turn path never allocates. A turn costs about
15 `millis()` reads, two heap probes and a few counter reads; the SD counters
are three 32-bit increments in `HalStorage` (`lib/hal/HalIoCounters.h`).

## Page-turn pipeline (2026-09-28)

`src/activities/reader/ReaderPageRenderer.*` is the page pipeline shared by the
reader and the host profiler: prewarm, BW render, status bar, BW refresh, tiled
grayscale, re-sync. Host profile of `吾輩は猫である` (123 chapters) with the
device settings (landscape CW, PocketSansWorld 12 as the SD font, wide spacing,
margin 5, AA on), 217 turns, median of 5 runs. Host CPU is roughly two orders
of magnitude faster than the ESP32-C3, so the counts matter more than the
milliseconds.

| per turn (host) | before | after |
|---|---|---|
| SD opens, within a chapter | 29.5 | 1.9 |
| SD sector loads, within a chapter | 254 | 196 |
| heap allocations, within a chapter | 227 | 153 |
| SD opens / sector loads, forward into the next chapter | 26.6 / 474 | 3.0 / 384 |
| layout on the turn into the next chapter | 1.19 ms, 2,068 allocations | none (pre-built; 164 allocations) |
| CPU: BW render / gray strips / status bar, landscape (ms) | 0.13 / 0.32 / 0.33 | 0.08 / 0.19 / 0.00 |
| CPU: gray strips, portrait (ms) | 0.46 | 0.36 |
| CPU per turn without the position save, landscape (ms) | 0.95 | 0.45 |

Frames are byte-identical (BW frame and both gray planes, every turn): the
book in all four orientations with AA on and off (217 turns each), and ten test
EPUBs in all four orientations. The profiler lives outside the repository (it
links the real `lib/` sources against file-backed HAL stubs); the firmware-side
checks are the host tests named below.

What changed:

- **Status-bar title glyphs.** A CJK chapter title is drawn with the reader's
  SD font, whose page glyph set is pinned during the page render, so every
  title glyph not on the page came from the font's 8-slot overflow ring: one
  file open (a FAT path walk) and ~2 sector reads per glyph per draw, ~26 opens
  per turn for `【一 】 (Part 1/5)`. The title now joins the page's sorted
  glyph batch (`FontCacheManager::recordExtraText`) while staying out of the
  page's kerning classes, so it renders exactly as before.
- **Chapter facts per spine.** The status bar and the position save read 5-8
  `book.bin` spine/TOC entries per turn; they are read once per chapter.
- **Grayscale strip culling.** The BW pass records each text line's physical
  ink rows; a strip pass skips lines that do not reach it (exact: gray pixels
  are a subset of BW pixels per glyph). Landscape lines align with strips; in
  portrait the per-glyph cull remains. Strips are 76 rows on the X3 (7 per
  plane, as before) instead of 80, 396 B less scratch.
- **Glyph blitter.** Page glyphs are drawn with the orientation transform and
  strip clip hoisted out of the pixel loop (`GfxRenderer::blitGlyph`).
- **Prewarm buffers** are sized to the page (codepoints, intervals) and the
  kerning rows reuse the prewarm's open font file.
- **Next chapter pre-build.** On a chapter's last page, after 150 ms idle and
  with at least 32 KB contiguous heap, `loop()` lays out the next spine one
  page per tick; the forward turn adopts it. Any other render first discards
  a build in progress, so a page render never overlaps it; low heap suspends
  it. Spines whose HTML still needs a >96 KB inflate are left to the turn.
- **Deferred position save.** `progress.bin` (create, remove, rename) is written
  1.5 s after the last turn or on exit instead of inside every turn.
- **Page loading** allocates each text line and its block together with their
  shared-pointer control blocks (two fewer allocations per line) and reserves
  the element list.

Memory (host-tracked heap, same 217 turns): the page render's peak is lower on
137 turns and higher by at most 0.4 KB on 39 (the title's glyphs join the page
set; the 76-row strip and the single-allocation lines pay for most of it). The
reading loop's highest point is still a chapter build, now usually in idle
time: the pre-build peaks where the turn's build did plus the shown chapter's
`Section` object (~0.2 KB), and only starts with 32 KB contiguous free, which
the turn path never checked. The recorder adds ~0.9 KB of heap while reading
and nothing elsewhere; static RAM +24 B, flash +8.5 KB.

Tests: `test/reader_perf` (record format and recorder),
`SdFontPageCache.StatusTextRecordedWithTheScanLoadsWithThePageGlyphs`,
`SdFontExtraKerning.*` (title glyphs keep their on-demand kerning),
`GfxHostInkRows.*` (gray pixels stay within the BW ink rows, 2-bit glyphs, four
orientations).

Measured, not changed:

- The segmented 8 KiB inflate window costs ~4% of inflate CPU against the old
  contiguous 32 KiB window (host: 18.4 vs 17.7 ms for all 123 chapters, 3.3 MB),
  well under a millisecond per chapter on the device.
- The remaining SD cost per turn is the prewarm (~190 sector loads: one glyph
  record and ~one bitmap sector per glyph, ~100 glyphs a page). 44% of a
  Japanese page's glyphs (74% for Latin text) were on the previous page, so
  keeping the previous glyph set would save ~40% of those loads, but it holds
  two glyph sets at once (+12-16 KB transient) and is left out.

## X3 telemetry and second pass (2026-09-28)

First device numbers (`0dcab16d`, 17 turns, landscape, AA on, PocketSansKR at
X Large): total 2,007 ms average; input 280 (max 890), section 252 (max 2,907
on a turn into an unbuilt chapter, ~4 ms when pre-built), prewarm 238, BW
render 29, status 8, BW refresh 445 (3,209 on the HALF turn), gray render 124,
gray refresh 227, gray sync 61, save up to 419; minimum free heap 20,096 B,
minimum largest block 12,276 B. `sdReads` (~1,000) counts `HalFile::read`
calls, mostly cached-sector copies while a page decodes; the host model puts
physical sector loads at ~190 per turn, nearly all in the prewarm (~1.2 ms per
single-block SD read on the X3).

Second pass (41aea867) and what the X3 then showed (25 turns, a German+Korean
aphorism book where nearly every turn enters a new spine): median ~2.4 s per
turn, input 598 ms, section 440-720 ms on consecutive turns (flags 3 and 2), AA
shown on 2 of 16 recent turns. Causes:

- Layout lost the race against the reader. Chapter layout ran from `loop()`
  one page per tick and the next-chapter pre-build waited for 150 ms idle and a
  32 KB contiguous block (largest block 13-35 KB on that run): no turn was
  ever pre-built, every turn into a spine inflated and laid it out on its own
  path, and `loop()` ticks holding the render lock delayed the next render
  (`input`).
- The adaptive cross-page glyph reuse held a second glyph set across turns
  (lower largest free block, more fragmentation) and could not be shown to
  reduce SD reads on the device's heap: removed.
- AA only yields to a real queued turn (`pageTurn()`), and every queued turn is
  followed by a render, so a page the reader stays on always gets its gray
  pass; the missing AA was the user pressing again during those slow renders.

Now (`ReaderLayoutAhead`): after each complete page (AA included) the render
task lays out the rest of the shown chapter and, from its last page, the next
chapter, one page per step, stopping as soon as `loop()` sees any button. The
forward turn adopts the section. The turn path lays out only up to the page it
shows. Pre-builds start with 16 KB contiguous (chapter builds allocate 8 KB
blocks). `test/reader_layout` reads `test/epubs/short-spines.epub` with two
pages of layout between turns, none, and idle pauses: every page is laid out
exactly once, every chapter turn with room is pre-built, a turn takes at most
one build step, and AA completes unless a turn is queued (and a cut pass
re-syncs the controller).

The 1-page landing layout and the idle position save outside the render lock
(41aea867) stay.

## Physics-bound cost (X3)

From `open-x4-sdk` (unchanged): the X3 controller runs SPI at 16 MHz, so one
52,272-byte plane takes 26 ms on the wire. A FAST turn sends the new frame and
re-syncs the old-frame RAM (2 planes, ~52 ms) around a 19-frame waveform
(`lut_x3_*_fast`); HALF uses 25 frames plus a 200 ms settle. Anti-aliasing adds
two gray planes (~52 ms), a 7-frame gray waveform (`lut_x3_*_gc`) and the BW
re-sync of both planes (~52 ms), plus the strip rendering above. The frame
period is not documented; the SDK's ~770 ms for a 62-frame FULL update puts it
near 8 ms, i.e. roughly 150 ms for the FAST waveform and 50-60 ms for the gray
one. `bwRefresh`, `grayRefresh` and `graySync` measure these directly.

Estimated from those constants, anti-aliasing costs about 150-200 ms per turn
(SPI ~105 ms, waveform ~55 ms, strip rendering) and a second visible panel
update. Turning *Text Anti-Aliasing* off is the largest single saving left;
it is a legibility trade-off for the reader to choose, not a default change.
*Refresh Frequency*: on the X3 a HALF refresh is a full resync (62-frame
waveform, conditioning pass and settle), 3.2 s measured, once every 15 turns
on the test reader (~0.18 s per turn on average); 30 pages halves that. SDK-side, the post-gray re-sync also rewrites the
new-frame RAM that the next FAST turn rewrites anyway (~26 ms per turn).
