# Preparing reader fonts ahead of a page turn — 2026-10-29

Both prototypes were rejected and their source changes reverted. Moving glyph
loading into the reading interval worked for English, but the final X3 pilot
improved total forward-turn p50 by only 2.7% in English and 1.0% in Korean with
images. Both had lower sampled free heap. Neither meets the existing screen.

## Mechanism and scope

The existing idle block in `EpubReaderActivity::loop()` scans the next page after
400 ms. Its `FontCacheManager::PrewarmScope` destructor then releases the glyph
cache, so the next page scans and loads again. The experiment retained those
existing allocations and handed them to one matching render through a consumed
token. It did not allocate a second framebuffer, change panel waveforms, modify
cache formats, import upstream development code, or enable a new user setting.

The key bound the token to the section object, spine, page and full SectionLayout.
Font/cache mutations invalidated reuse; a miss took the ordinary scan path.
Retention required at least 60,000 bytes free and a 16 KiB largest block after
preparation. This is an admission check, not continuous peak-memory proof.
The experiment was enabled only during automated developer reader walks. Normal
menus, physical-button reading, sleep and battery behavior were not acceptance
claims. A successful pilot would still need those lifecycle paths completed and
tested before general use.

The first prototype prepared only inside the shown chapter. English benefited,
but the Korean/image fixture changes chapters on every page and could not use
that path. The second borrowed a completed, matching neighbour from the existing
ReaderLayoutAhead object, then relied on adopt() preserving its identity. It
prepared the next chapter's title as well. No additional chapter copy or layout
work was introduced.

## Fresh X3 measurements

Each walk rendered 16 pages. Opening was excluded from forward-turn p50. A fresh
baseline was followed by two candidates, with original-firmware restoration
between them. This is an exploratory A/B1/B2 sequence sharing one baseline;
it is not five independent paired AB/BA repeats or a production acceptance run.

| Fixture | Baseline | Same-chapter candidate | Cross-chapter candidate | Final gain |
| --- | ---: | ---: | ---: | ---: |
| English | 1,203 ms | 1,183 ms | 1,171 ms | 2.7% |
| Korean with images | 1,753 ms | 1,755 ms | 1,735 ms | 1.0% |

For the cross-chapter candidate:

| Metric | English, before → after | Korean/images, before → after |
| --- | ---: | ---: |
| Recorded work through BW, p50 | 578 → 552 ms | 1,262 → 1,243 ms |
| `prewarm` stage, p50 | 25 → 0 ms | 189 → 169 ms |
| SD reads, p50 | 107 → 14 | 160 → 154 |
| Sampled minimum free heap | 61,128 → 61,020 B | 58,812 → 58,772 B |
| Sampled minimum largest block | 42,996 → 42,996 B | 47,092 → 42,996 B |

Both candidates' final BW captures were byte-identical to their baseline for
both fixtures, with matching per-record render flags and strip counts. This
covers four comparisons, not every intermediate page or optical grayscale.
The six walks rendered 96 pages total. X4 was not modified or tested in this
experiment because the X3 pilot already failed the gain and memory screen.

## What the measurement establishes

English's zero-millisecond median prewarm and lower SD counter demonstrate that
work moved outside the recorded turn. They do not imply zero preparation cost,
lower battery use, or a similar reduction in the complete turn.

The `prewarm` stage is not a pure font-I/O timer. ReaderPageRenderer::renderPage
also checks/decode-prepares cold images before its glyph scan. The Korean pilot
still spent 169 ms in that combined stage; this trace cannot assign that remainder
to image decoding, cache invalidation, or glyph work separately. Do not report
the original 189 ms as fully removable font loading or a measured decoding time.

The image fixture also spends about 980 ms in two BW updates; its grayscale
refresh and synchronization add more work. The next useful investigation is to
split image preparation from glyph preparation in measurement, then evaluate a
single combined grayscale update or earlier composition/controller staging as
separate hypotheses. The single-framebuffer, capture, gray encoding and panel
conditioning obstacles are documented in the second-perspective report. No
predicted panel speedup or optical quality is claimed here.

## Software validation and preserved evidence

Both prototypes passed strict cppcheck 2.11 and warning-free default firmware
builds. Complete host tests passed 1,155 for the first and 1,156 for the second.
Nonuniform real-font pixel tests covered all four orientations and both display
polarities, one-shot reuse, normal fallback and unchanged framebuffer during
preparation. Three injected faults were detected: disabling cache handoff,
accepting the wrong page, and accepting a neighbour with the wrong layout.
After restoring the source, all 1,149 original host tests passed again.

ESP32-C3 compiled reader-loop frames were 128 B before and 144 B for both
candidates; the renderContents frame stayed 112 B. These are individual compiled
frames, not measured total task stack or heap peaks. The first targeted build
failed because a test called a private renderer method; the corrected test uses
a public invalidation path. That failed attempt remains in the evidence.

Ignored private evidence: `build/reader-prepared-fonts-20261029/`. Immutable
`baseline.bin`, `candidate.bin`, and `candidate-v2.bin` have SHA-256/version
manifests beside them. `candidate.patch` and `candidate-v2.patch` preserve the
reverted experiments; source hash manifests and `before/` preserve provenance.
`comparison.json` and `comparison-v2.json` retain both rejected comparisons.
All raw traces, BMPs, reports, validation logs and restoration attempts remain.

Source restoration and the restored local baseline staging image have separate
receipts. Final `restoration/closure.json` verified the original developer version, public
state, raw-five records and full contents of all 16 original books. No owned
workers remain. This is scoped restoration, not full SD or full-flash readback.
No commit, push, tag, upstream import or release was performed.
