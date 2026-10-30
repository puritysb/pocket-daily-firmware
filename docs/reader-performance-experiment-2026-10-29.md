# Reader performance experiment — 2026-10-29

The first physical optimization loop completed its timing exploration on X3 and
X4. The scaled-raster candidate was **rejected before the full five-pair campaign**:
observed p50 reductions were 0.9–6.0%, below the 10% screen. Its firmware changes
were removed from the working tree. No waveform, refresh cadence, antialiasing
quality, cache format or production release was changed.

## Experiment and results

Baseline: current `99f844c2` source, including the recent reading-memory repair,
with the bounded PDBM instrumentation. English is the unchanged original EPUB
fixture; the Korean fixture includes images. Each case opens a fresh path-keyed
cache and renders 16 pages with the existing one-second dwell. Opening is kept
separate; the table reports the median of the following 15 turns.

| Model | Fixture | Baseline p50 | Candidate p50 | Observed reduction |
| --- | --- | ---: | ---: | ---: |
| X3 | English text | 1,203 ms | 1,131 ms | 6.0% |
| X3 | Korean with images | 1,752 ms | 1,735 ms | 1.0% |
| X4 | English text | 1,103 ms | 1,049 ms | 4.9% |
| X4 | Korean with images | 1,865 ms | 1,848 ms | 0.9% |

These are **one paired exploratory trial per case**, not a repeatability claim.
There were eight successful walks / 128 rendered pages. No five-pair acceptance
screen was completed; no result was relabeled as a production improvement.

The candidate replaced per-pixel division and coordinate multiplication in
`glyphBitmap::drawScaled` with exact quotient/remainder stepping and flat bit
addressing. It added no heap buffer. Independent pixel-reference tests covered
11 scales, orientations, text rotations, bit depths, planes, polarity and clips.
Carry-boundary and clipped-prefix mutants both failed those tests.

X3 English BW rendering fell from 63 to 41 ms and grayscale rendering/transfer
from 287 to 245 ms (stage medians). However, BW refresh, grayscale refresh and
controller resynchronization remained approximately 480, 228 and 98 ms. X4
English's corresponding refresh stages were 631, 144 and 43 ms. The Korean
image fixture requires the existing double-refresh image path, further limiting
the proportion affected by glyph arithmetic. Do not extrapolate it to all
Korean text, and do not equate firmware completion time with optical response.

The controller resynchronization is real display-RAM work, not an arbitrary
sleep to remove: `Uc8253X3Driver::cleanupGrayscaleBuffers` writes both BW planes,
and `Ssd1677Driver::cleanupGrayscaleBuffers` writes the differential baseline to
RED RAM. Removing these writes would change the next refresh's starting state;
the timing trace alone does not justify doing so.

Sampled free-heap/largest-block minima did not worsen in these cases. The C3
compiler outlined the larger scaled routine: the old glyph caller was 240 B;
the candidate caller/helper were 208/112 B, a combined 80 B increase. No runtime
stack-watermark or long-session guarantee is inferred from those frame sizes.

## Output, failures and recovery

X4 final frame bytes matched in both fixtures. Each X3 comparison differed at
28 pixels in the charging indicator only: `(14,774)` through `(18,781)` on the
528×792 capture. Inspection showed a full battery versus its charging glyph;
the book body and remaining frame bytes matched. The whole-frame screen was
not silently weakened or marked passed. This also identifies a source of
dynamic-status noise for future benchmark design.

The first X4 candidate Korean attempt failed **before upload or submission**:
`GET dev/build-failure` returned the exact 409 `Reader is drawing` response.
That failure remains in its original report. After checking there was no pending
operation/owned fixture, a fresh run ID completed the Korean trial. This was not
a replay of the failed plan and is recorded separately.

The retained source fix adds only that diagnostic GET to the existing bounded
retry policy: exact route/method/status/body, at most six requests and ten
seconds. POST/DELETE, unknown responses and transport failures retain their
existing no-replay behavior. Tests explicitly guard the new GET and forbid
mutation retries; disabling the GET allowance or enabling mutation retries
causes the corresponding tests to fail.

Both original developer images were restored (`wb05eda4a` on X3,
`wd4859597` on X4). Public state and the raw-five records were checked; the empty
library invalidation marker introduced by owned fixture changes was removed
only after verifying the original marker was absent and the other four raw
records were unchanged. Full original-book readback closure is recorded in
`build/reader-performance-loop-20261029/restoration/closure.json`.
This is scoped restoration, not whole-SD, inactive-slot or full-flash equality.

## Evidence and next decision

Ignored evidence root: `build/reader-performance-loop-20261029/`.

- `experiment.json`: exact artifact hashes/versions, all four paired results,
  stage summaries, pixel comparisons, retained failure and decision.
- `baseline.bin`, `candidate.bin`, `rejected-candidate.patch`: immutable
  experimental inputs; the rejected patch is not applied to current source.
- `restoration/`: original-version, public/raw-five and whole-book readback.
- `mutations/`, `stack/`, build/test logs: host 1,149 and Python 309 passed,
  strict cppcheck 2.11 passed, both builds had no warnings. Software gates do not
  establish optical quality or physical-button/energy behavior.

The loop has demonstrated measurement, rejection and recovery, rather than an
extreme speedup. The next useful baseline separates pure Korean text from image
pages and investigates SD font prewarm independently. Retaining glyph caches
across pages needs its own fragmentation/headroom evidence. Large changes to
panel refresh require optical ghosting/contrast validation, which the current
digital-capture pipeline does not supply. Keep stable upstream integration and
the existing reading-memory improvements while evaluating those hypotheses.
