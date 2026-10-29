# Bounded SD font access for live content views

Status: live cards and legacy Sync layout previews use this mode through the
font-system facade. Actual Home/powered-off Brief previews (PDLI schema2) use
the normal Cached/fallback path with canonical PocketSansWorld12 UI selection;
a resident reading font must not change Home layout. EPUB settings are preserved. Exact-image X4 evidence is recorded in the sibling
app's `docs/X4_SETTINGS_ACCEPTANCE.md`; local I/O measurements below are separate
from physical Apply latency.

## Why it is needed

File Transfer explicitly unloads the resident SD font before starting its server
(`CrossPointWebServerActivity::startWebServer`). Calling the existing Pocket
card renderer from a live server would call `UiCjkFont::fontForText`, which can
reload a font, build advance tables and prewarm page-sized glyph data. Merely
separating the card snapshot from the activity does not bound those allocations.

`SdCardFont::LoadMode::BoundedUI` reuses CPFONT v4, EpdFont, glyph metrics and
GfxRenderer's existing glyph bitmap path. It does not introduce another font
format, framebuffer, renderer or network service. Font interval metadata is
validated sequentially at load and binary-searched on SD on demand. Kerning
class maps/matrix values and ligature pairs are also queried on disk via optional
EpdFontData callbacks, so the mode does not silently disable Latin shaping.
The new callbacks are runtime-only fields, not an on-disk layout change.

## Resource and ownership contract

- No interval, page-glyph, advance, kerning or ligature tables are allocated by
  this mode. `prewarm`/`buildAdvanceTable` check coverage through the bounded
  glyph cache instead of building page caches. `hasAdvanceTable()` stays false.
- The existing eight-slot overflow cache retains at most 8 × 256B of bitmap
  payload, plus one <=256B replacement until its read succeeds. The font object,
  heap allocator overhead, transient HAL handles, renderer registration and
  other font/render allocations are **additional**, not part of this bound.
  The font object gains one pointer (4B on the C3 ABI); host sizes are not
  ESP32 ABI evidence.
- Since the 2026-10-18 performance change, BoundedUI also owns a read state
  only between load and unload: 64 × 32B metadata blocks (2,048B), their
  offset/LRU tags (512B), two HAL handle members and a generation counter.
  `static_assert` limits this allocation to2,816B on every compiled target.
  The fake-HAL arm64 host allocation is2,680B; the real HAL's two underlying
  file implementations and allocator overhead are additional. Cached mode
  allocates none of this state. No static pool or whole-font table is added.
- Glyphs must fit64×64 and256 bitmap bytes, have the exact packed pixel length,
  and address bytes inside the loaded file size. This is a small-UI mode, not a
  replacement for arbitrary-size EPUB typography. Unsupported glyphs fail.
- Optional progress callbacks run during interval validation/search and shaping
  requests. They may feed the watchdog, never service file mutations. They do
  not bound a stalled individual SD call or establish latency acceptance.
- Bitmap replacement is fallible and keeps the previous cache entry on I/O or
  allocation failure. `boundedReadFailed()` latches disk/shaping/bitmap failures
  until a new load; a presentation must not certify a frame after that flag is
  set. Missing character coverage must also be checked explicitly.
  A metadata refill becomes valid only after a complete read. `clearCache`
  invalidates its blocks, and every load discards both handles and all old
  metadata. Allocation/open failures unwind the entire new state.
- Owner provides rendering exclusion and immutable font files during load/use.
  HAL serializes individual accesses, not the whole presentation transaction.
  This does not authenticate a font or prove arbitrary external SD edits safe.

## Library metadata glyphs

Library explicitly uses the existing single family slot for PocketSansWorld12
in BoundedUI mode. The renderer keeps the built-in UI face for covered glyphs
(including Latin); missing glyphs borrow this bounded face through the existing
per-glyph provider. This is separate from the reader's PocketSymbols fallback.
Network BoundedUI loads still disable the provider.

Library enters this mode under its render lock after index preparation and
releases it on exit. Provider mode is cleared before the manager unregisters
and frees the font; it does not retain a pointer across a family switch. Reader
entry reloads the normal Cached family, and saved reader family/point size are
unchanged. Re-entry can reuse the same bounded font. Missing or unreadable font
loads leave the provider disabled and retain the renderer's missing-glyph marks.
The existing bounded metadata state and bitmap ring above are the allocation
budget; no second family, interval/page/advance table, or new cache is added.

This addresses a confirmed X3 distinction: the checked index/title and actual
row/open protocol succeeded, but the Library frame displayed five missing-glyph
marks for `Pocket RC 한국어 읽기`. A normal Korean reader page did not establish
Library glyph correctness. Native lifecycle and raster checks are separate from
the next candidate's physical Library frame and heap acceptance.

## Local evidence

The SD-font host target compiles the actual SdCardFont and EpdFont sources with
a fake HAL. It checks both cached and bounded paths, UTF-8 coverage, ring reuse,
read/allocation failure, offset/interval/bitmap bounds, progress callbacks,
sticky failure, kerning and ligature equivalence. Existing host tests remain in
the full regression set.

An explicit fixture tool checks the locally staged production font, not a
mounted reader:

```sh
build/host-tests/sd_font/sd_font_fixture_check \
  firmware/sd-card/.fonts/PocketSansWorld/PocketSansWorld_12.cpfont
```

It compared71,036 regular/bold glyph metrics and bitmap payloads byte-for-byte
between Cached and BoundedUI, plus sampled Latin shaping. This is not exhaustive
shaping, rendered-page parity, SD performance, live-network or panel evidence.
The current bundle has119 intervals per style (not tens of KB of interval
metadata); bounding page caches and shaping tables matters as well as intervals.

## Live integration and remaining acceptance

The facade now requires the same load mode before reusing a font family. The
live content page uses the preflighted BoundedUI font directly, avoiding implicit
Cached reloads through UiCjkFont. The presenter releases the font after each
paint before admitting another transfer, and reloads on explicit navigation.
See [content-display.md](content-display.md) for ownership and receipt semantics.
Controller failure tests cover the production state machine with fake font and
display boundaries. Actual memory headroom, font-facade failures, SD latency and
physical display acceptance are recorded per image in the sibling acceptance
document; framebuffer results do not establish optical panel quality or minimum
render-time heap. No automatic session end or firmware upload belongs in this path.

## Card render I/O reduction — 2026-10-18

`SdCardFont::readBounded` keeps two read-only HAL handles for the loaded font:
metadata and bitmaps. Repeated glyph lookup, interval search, kerning and
ligature lookup no longer reopen the path. The separate bitmap cursor avoids
rewinding the metadata cursor for each glyph, and bitmap traffic cannot evict
metadata blocks. The 32B fully associative LRU blocks fit packed 3B classes,
8B ligatures, 12B intervals and 16B glyph records without aligned-load casts.
Reads spanning a block boundary copy each part into the existing caller record.
The existing eight-entry bitmap ring and its fallible replacement are unchanged.

The small block choice was measured against32/64/128/512B alternatives at the
same2,048B payload bound. The 512B variant read12,717,386B for the workload
below; the selected32B variant read324,970B. A filesystem sector-sized cache
was ineffective for sparse kerning matrix entries. These are host HAL traffic
counts, not device bus transfers or elapsed time.

Local reproducible artifact: `build/bounded-font-perf/fixture-before.txt` versus
`fixture-after.txt`, using `test/sd_font/SdFontFixtureCheck.cpp` with the deployed
PocketSansWorld12 font. The fixture compares71,036 regular/bold glyph metrics
and bitmap payloads byte-for-byte, then measures three English/Korean/Japanese
text samples in both styles,12 repeated bounds passes each (72 measurements).
Every measured dimension agrees with Cached shaping. This is a font-level
workload, not the36-card Apply workflow.

| Measurement phase total | Prior code | New code |
|---|---:|---:|
| File open attempts |13,064|0|
| HAL read/seek calls |84,728|6,189|
| HAL requested/read bytes |714,394|324,970|

The new font load itself opens two handles; the table excludes load in both
versions. Reads fall92.7% and requested bytes54.5%. These host counts alone do
not establish X4 latency. The separate final integration run below compares the
same36 cards, exact preview pixels, reboot persistence and post-presentation
heap using the recorded firmware artifact.

Focused evidence in `build/bounded-font-perf/`:

- `tests.xml`:19 SD-font tests, including four new cache/ownership/eviction
  cases. Nonuniform two-style fixtures cover192 glyphs and18,432 pair lookups
  each for kerning and ligatures; cross-block records, failed refill, OOM,
  failed second open/seek, retained old bitmap, reload and mode switch are covered.
- `cached-tests.log`:6 existing cached-font tests; `reader-layout-tests.log`:
  10 existing EPUB layout tests (compiled with the new SdCardFont source).
- Four separately compiled fault variants all fail: `no-cache` violates the
  I/O bound; `wrong-offset` breaks metrics/pixels; `partial-hit` accepts a
  failed refill; `stale-clear` misses corrupt shaping after explicit invalidation.
  Mutants are ignored copies; production sources were never fault-injected.
- arm64 `-O2 -fstack-usage` reports112B for `readBounded`,80B for class/ligature
  helpers,96B for kerning/interval lookup,176B for the existing glyph miss path.
  These are host compiler frames, not ESP32 stack high-water measurements.

The existing ContentTextLayout whole-word fix, CJK break decisions, drawing
semantics, CPFONT format and preview protocol were not changed by this font
optimization. Full firmware/static-analysis and physical results follow below.

### USB logging during physical performance tests

An integration run investigated another latency path: with USB enumerated but the
monitor closed, the second rotated card repeatedly timed out at preparation;
ICMP and TCP handshake still worked. Draining CDC through a raw file descriptor
(without DTR/RTS calls) produced no boot marker and resumed main-loop log
timestamps after a long gap. An open monitor had allowed the same cards to pass.
Thousands of per-glyph debug records exercise serial backpressure independently
of SD caching. Espressif documents the closed-port/full-buffer behavior in the
[ESP32-C3 TRM, section 30.4](https://documentation.espressif.com/esp32-c3_technical_reference_manual_en.pdf).

`lib/Logging/Logging.cpp` now admits serial output only if the whole record fits
the current TX capacity. A non-waiting atomic writer flag prevents competing
logger calls from racing that check. Congested/disconnected/nested output is
skipped, while the existing bounded RTC diagnostic ring remains populated.
There is no new heap allocation, timeout change, persistent queue or global
Arduino driver patch; the ROM transport is unchanged. This makes logging
best-effort without requiring an attached USB monitor for normal operation.
The local Arduino HWCDC already contained machine-local driver changes before
this task; they were inspected, not modified. Acceptance is of the recorded
binary, not an untested CI or stock-driver build.

Four production-logger host regressions cover a 10,000-record saturated burst,
disconnect/recovery, exact capacity boundary, reentry and 255-byte truncation.
An isolated mutant removing the capacity check fails these checks. Evidence:
`build/connected-preview-logging-tests.xml`,
`build/connected-preview-log-fault/result.json`. Final physical acceptance ran
with no USB log connection and records its exact installed image below.

Logging admission alone did not resolve the full run: `20261017T163444-927ed6`
failed after ten cards. Retained RAM showed card navigation queued behind a
Saved transfer banner. The activity now clears that banner for any busy
presentation after transport release, including navigation with no deferred
preparation. The first JTAG attachment reset the core, so its stacks are not
uninterrupted-execution evidence. The app acceptance document retains these
failures, the revised diagnosis and the subsequent full-run result.

### Final integration acceptance — 2026-10-18

Full `--x4-starters` run `20261017T165221-1e51ac` **passed** on installed
`0.1.0-dev-main-3f0bade2-w8014224c` (6,431,552 B, SHA-256
`43fa3e68894d7a39203afa42bf2f775d3c1fc4a5d3789b4468f52bc4238e886a`).
USB monitor/JTAG stayed closed. All 36 card frames matched the app preview;
12 collection and seven preset software reboots retained state. Four Home and
three Brief presets matched complete connected previews before/after reboot.
Independent Pillow verification found zero differences in all69 pairs; all13
contact sheets were visually reviewed. Original state was restored.

Compared with the same36 card IDs in `20261017T141105-0f7362`, the complete
Apply/read-back/presentation median fell20.82→10.23s (**50.8%**); the range
fell12.77–37.96→4.82–12.06s. This measures the combined final integration,
not a hardware A/B attribution to the cache alone. The43 post-presentation
health probes reported36,032–39,648B free heap and no crash report; they do not
establish minimum render-time heap, largest-block headroom or long-term leaks.
Host973/Python139/strict cppcheck2.11 and warning-free default build passed.

The sibling app's [acceptance record](../../pocket-daily/docs/X4_SETTINGS_ACCEPTANCE.md)
owns hashes, evidence paths and failed trials. Exact preview scope is Sync
Apply with its placeholder cover/Brief footer, not ordinary cover decoding,
optical panel appearance or actual deep-sleep/wake.

### Library display normalization — 2026-10-26

Library rows compose copied title, subtitle and section-heading buffers with
`utf8ComposeNfcInPlace` before width measurement and truncation. This reuses the
existing buffers without growing them; decomposed Hangul filenames therefore
render as composed syllables with the bounded World UI fallback. Raw metadata
still drives grouping, selection and the developer trial's expected-title hash.
Paths, index records, book bytes and persisted reading identity are unchanged.
The native checks use public synthetic text for NFC/NFD pixel and truncation
parity; they do not establish physical-panel appearance or change old trial
results.
