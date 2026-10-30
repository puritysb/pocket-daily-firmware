# Second perspective on page-turn latency — 2026-10-29

The next useful direction is to change **when a page is prepared**, or the
number of panel updates required, rather than keep tuning small CPU loops.
Neither architectural change is implemented or accepted by this experiment.

## A second measured hypothesis

`SdCardFont::prewarmStyle` sorts reads but reads each glyph separately. This
candidate grouped adjacent metadata records only when their source and
destination were both contiguous. It grouped adjacent bitmap ranges directly
into the existing packed bitmap, stopping at gaps, overlaps and empty glyphs.
No heap buffer or retained cache was added; the C3 function frame remained
144 bytes. The isolated synthetic 200-glyph page used 4 reads instead of 402,
with the same metadata and bitmap contents.

A fresh X3 paired pilot used the same immutable baseline as the first
experiment, a new candidate, fresh path-keyed caches and new run IDs. Each
fixture rendered 16 pages; opening was excluded from forward-turn medians.

| Fixture | SD reads p50, before → after | Total p50, before → after |
| --- | ---: | ---: |
| English | 107 → 57 | 1,203 → 1,203 ms |
| Korean with images | 160 → 160 | 1,752 → 1,754 ms |

One pair per fixture is exploratory, not repeatability evidence. English
prewarm remained 25 ms and Korean prewarm 189 ms. The Korean page's needed data
did not benefit from adjacent-range batching. Sampled free heap was also 20/24
bytes lower, so this did not pass the unchanged conservative screen. The source
patch was rejected and preserved as evidence; no X4 candidate trial was needed
after the X3 pilot failed. X4 received a fresh read-only backup only.
Both final X3 BMPs were byte-identical to their paired baseline, and all render
flags/strip counts matched. This does not establish grayscale optical equality.

Candidate host tests: 1,152 passed; strict cppcheck 2.11 passed; default firmware
build had zero warnings. Two injected faults were detected: disabling metadata
batching and shifting bitmap destinations. The pixel tests included nonuniform
bytes, reversed source order, gaps, overlaps, empty glyphs and short reads.
An intermediate host run accidentally reused a mutant object; forcing a source
rebuild restored the clean 1,152-test result. The initial analyzer invocation
used bundled cppcheck 2.20 despite its config override; the verified
`scripts/check_firmware.py --cppcheck build/native-cppcheck/cppcheck` run passed.
Both failed validation attempts remain in the evidence directory.

## Measure the waiting interval before choosing the optimization

The first experiment's baseline traces already separate the recorded work
through BW refresh from later grayscale work:

| Model / fixture | Recorded work through BW, p50 | Complete turn, p50 |
| --- | ---: | ---: |
| X3 English | 578 ms | 1,203 ms |
| X4 English | 712 ms | 1,103 ms |
| X3 Korean with images | 1,260 ms | 1,752 ms |
| X4 Korean with images | 1,541 ms | 1,865 ms |

These values sum each record's input/section/page/prewarm/BW/status/refresh
durations before taking medians. They do not measure optical legibility, input
polling or every excluded interval. `scripts/reader_benchmark.py` now reports
these work totals and per-turn I/O counters without changing acceptance rules.
A regression test catches inclusion of the gray tail, opening, and the invalid
shortcut of summing stage medians. An injected gray-boundary fault was detected.

## Architectural candidates and the concrete obstacles

1. **Prepare the next screen during reading.** `ReaderLayoutAhead::afterPage`
   already builds page layout while idle (`ReaderLayoutAhead.h`,
   `EpubReaderActivity.cpp::layOutAhead`). Repeating that optimization will not
   help. The new scope would be glyph loading, BW composition and possibly
   controller-RAM upload before the next button press. Prototype only adjacent
   forward text pages first; use the ordinary path on every miss.

   One framebuffer means preparing the next frame overwrites the representation
   of the currently visible page. `TrialReader.cpp` and `dev/ScreenCapture.cpp`
   currently capture that buffer directly. A correct implementation needs an
   explicit displayed/prepared state with a page/layout/font/refresh-generation
   key, cancellation on backward turn, menu, settings, sleep and activity exit,
   and capture that cannot report the prepared frame as already displayed.
   It must not solve this with another full framebuffer. Read-ahead must yield
   to input and must account for wasted work and battery cost.

   The driver split is also incomplete for this purpose: `displayStart()` in
   `Uc8253X3Driver` and `Ssd1677Driver` uploads RAM **and starts the waveform**.
   It is not a prepare-only API. The X3 finish path still consumes the host BW
   frame. Introduce and test a prepare/activate/cancel HAL contract before trying
   speculative controller writes. Preserve refresh cadence and old-plane state.

2. **Compose a four-level page and update once.** The reader currently uses BW
   plus grayscale overlay; image pages first use two BW updates
   (`ReaderPageRenderer.cpp:155`). The local SDK advertises combined direct gray
   for the X3 and combined absolute gray for the X4, with different contracts
   (`Uc8253X3Driver.h::grayscaleCapabilities`,
   `Ssd1677Driver.h::grayscaleCapabilities`). These capabilities are leads, not
   current reader quality or speed evidence. Measure a full mode-specific path,
   including exit/return conditioning, rather than subtracting today's BW
   duration and calling it a gain.

   Font rendering also needs work: `renderCharImpl` currently forces BW glyph
   selection for absolute planes. Simply selecting the SDK mode would not
   preserve the reader's antialiasing. Independently verify both output planes,
   all four gray levels, status polarity, all orientations, cancellation and
   transitions back to overlays. Then measure actual panel ghosting/contrast.
   The present framebuffer-capture pipeline cannot provide that optical gate.

The first candidate changes scheduling with the existing waveform; the second
changes the panel update contract. Keep them separate, preserve ordinary-path
fallbacks, and validate end-to-end output and recovery before adoption. No
predicted percentage here is a measured speedup.

## Evidence and restoration

Ignored evidence: `build/reader-io-batching-20261029/`. `comparison.json` records
all four successful walks (64 pages), exact BIN identities are in
`baseline.json` and `candidate.json`, and `rejected-candidate.patch` preserves
the unapplied experiment. `previous-bw-stage-analysis.json` references the first
experiment's baseline records. Raw timing/BMP/report files remain in the walk
directories. Recovery completion is recorded separately in
`restoration/closure.json`; use that receipt, not an intermediate trial state.
The final closure confirmed both original developer versions, public state,
raw-five records and full contents of all 23 original books. No owned workers
remain. This is scoped restoration, not full-SD or full-flash readback equality.
The retained summary changes passed all 310 Python tests; after reverting the
experimental C++ patch, the complete 1,149-test host suite passed again.
No commit, upstream import or publication was performed.
