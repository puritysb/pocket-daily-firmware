# Bounded SD font access for live content views

Status: implemented and host-verified loader mode, now selected by the live
content presenter through a mode-aware font-system facade. The default Cached mode and
EPUB reader behavior remain unchanged. No device deployment is implied.

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
  Host `sizeof(SdCardFont)` is2544B; do not use that as ESP32 ABI evidence.
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
- Owner provides rendering exclusion and immutable font files during load/use.
  HAL serializes individual accesses, not the whole presentation transaction.
  This does not authenticate a font or prove arbitrary external SD edits safe.

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
physical display acceptance remain outstanding. No automatic session end or firmware
upload belongs in this path.
