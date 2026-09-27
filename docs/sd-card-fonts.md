# SD Card Fonts

CrossPoint supports loading additional fonts from the SD card, including fonts
with extended Unicode coverage (CJK, Cyrillic, Greek, etc.).

## Installing Fonts

There are three ways to install fonts:

### Option 1: Download from device (recommended)

1. Connect your CrossPoint reader to Wi-Fi
2. Go to **Settings > System > Manage Fonts**
3. Browse available font families and tap to download
4. Downloaded fonts appear immediately in **Settings > Reader > Font Family**

### Option 2: Upload via web browser

1. Start **File Transfer** and connect through **Join Network** or **Create Hotspot**
2. Open the web interface URL shown on the reader
3. Navigate to the **Fonts** tab
4. Upload `.cpfont` files using the upload form

### Option 3: Manual SD card copy

1. Download font files from the
   [crosspoint-fonts repository](https://github.com/crosspoint-reader/crosspoint-fonts)
2. Copy font family folders to one of two locations on your SD card:

   - `/.fonts/` — hidden directory (preferred; keeps the SD root tidy
     when mounted on a desktop)
   - `/fonts/` — visible directory (use this if your OS hides dot-files
     and you'd rather see the folder in your file manager)

   Both roots are always scanned at boot and the results are merged: a
   family installed in `/fonts/` shows up even when `/.fonts/` also
   exists, and vice versa. The two roots only collide if the same family
   name appears in both — in that case the copy in `/.fonts/` wins and
   the duplicate in `/fonts/` is ignored.

       SD Card Root/
       ├── .fonts/                     ← Hidden root (preferred)
       │   └── Literata/
       │       ├── Literata_12.cpfont
       │       ├── Literata_14.cpfont
       │       ├── Literata_16.cpfont
       │       └── Literata_18.cpfont
       └── fonts/                      ← Visible root (equally valid)
           └── Merriweather/
               ├── Merriweather_12.cpfont
               └── ...

3. Insert the SD card and power on your CrossPoint reader

## Available Pre-Built Fonts

The current list of pre-built fonts is maintained in the
[crosspoint-fonts repository](https://github.com/crosspoint-reader/crosspoint-fonts).

## Converting Custom Fonts

To convert your own TrueType/OpenType fonts:

### Prerequisites

    pip install freetype-py fonttools

### Single font (one style)

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py \
      MyFont-Regular.ttf \
      --intervals latin-ext \
      --sizes 12,14,16,18 \
      --style regular \
      --name MyFont \
      --output-dir ./MyFont/

### Multi-style font

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py \
      --regular MyFont-Regular.ttf \
      --bold MyFont-Bold.ttf \
      --italic MyFont-Italic.ttf \
      --bolditalic MyFont-BoldItalic.ttf \
      --intervals latin-ext \
      --sizes 12,14,16,18 \
      --name MyFont \
      --output-dir ./MyFont/

### Available Unicode interval presets

| Preset | Coverage |
|--------|----------|
| `ascii` | U+0020–U+007E (Basic Latin) |
| `latin1` | U+0080–U+00FF (Latin-1 Supplement) |
| `latin-ext` | European languages (Latin + Extended-A/B + punctuation + ligatures) |
| `greek` | Greek + Extended Greek |
| `cyrillic` | Cyrillic + Supplement |
| `hebrew` | Hebrew + Alphabetic Presentation Forms |
| `georgian` | Georgian + Georgian Supplement |
| `armenian` | Armenian |
| `ethiopic` | Ethiopic + Extended |
| `vietnamese` | Vietnamese subset (ơ/ư and combining marks) |
| `punctuation` | General punctuation (U+2000–U+206F) |
| `cjk` | CJK Unified Ideographs + Hiragana + Katakana + Fullwidth |
| `hangul` | Korean Hangul syllables + Jamo + Compatibility Jamo |
| `cherokee` | Cherokee (historic + supplement block) |
| `tifinagh` | Tifinagh |
| `symbols` | Math, currency, arrows, box-drawing, misc symbols, dingbats |
| `reading` | Literary fiction coverage: Latin, Greek, Cyrillic, math/symbol blocks, supplemental punctuation, and CJK quote marks |
| `builtin` | Matches the firmware's built-in font conversion intervals |

Combine presets with commas: `--intervals latin-ext,greek,cyrillic`

You can also specify arbitrary Unicode ranges directly:
`--intervals latin-ext,(0x2100-0x214F)`

To list all presets with codepoint counts:

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py --list-presets

### Additional options

`--force-autohint` — force FreeType's auto-hinter instead of the font's native hinting (useful when a font's built-in hints produce poor results at small sizes).

Install custom fonts via the web interface or manual SD card copy.

## Missing glyphs, invisible characters and the glyph-fallback font

The reader never draws a box per codepoint for text its font cannot show.

### Invisible characters

Every `Default_Ignorable_Code_Point` (Unicode `DerivedCoreProperties.txt`: ZWJ/ZWNJ
U+200C/D, variation selectors U+FE00–FE0F and U+E0100–E01EF, tag characters
U+E0020–E007F, bidi controls, word joiner, BOM, Hangul fillers, …) and stray emoji
skin-tone modifiers U+1F3FB–1F3FF are drawn with zero width and never looked up in a
font, in layout measurement and in rendering (`utf8IsInvisible`, `lib/Utf8`). Break
opportunities inside a word never precede one of them or follow a ZWJ. The source
text is not modified: KOReader sync offsets and XPaths still count every codepoint.

### Emoji clusters

An emoji sequence renders as one glyph: the first pictograph of a ZWJ sequence
(👩🏽‍💻 → 👩), the first regional indicator of a flag, a base followed by variation
selectors, skin tones or tag characters (`utf8SkipEmojiClusterTail`).

### Fallback chain

For each visible codepoint the renderer tries, in order:

1. the text's font (SD `.cpfont` or built-in);
2. the glyph-fallback family `PocketSymbols`, if installed;
3. a small dotted outline, one per grapheme cluster, drawn in the black-and-white
   pass only (its width is `max(4, lineHeight × 5 / 12)` px, consistent between
   layout and drawing). An uncovered combining mark adds nothing to its base.

Install the fallback font at:

    /.fonts/PocketSymbols/PocketSymbols_12.cpfont

(`/fonts/PocketSymbols/` also works). The family is discovered at boot like any other
but kept out of the reader-font list. The firmware build stages it, with its three
OFL notices, under `firmware/sd-card/.fonts/PocketSymbols/`; the source asset,
provenance, license verification and rebuild command are in
`assets/fonts/PocketSymbols/README.md` (`scripts/build-pocket-symbols.py`). It holds
3,888 monochrome emoji and symbol glyphs from Noto Emoji, Noto Sans Symbols 2 and
Noto Sans Math (all SIL OFL 1.1) in 413 KB.

Resource rules:

- The fallback loads lazily in Cached mode the first time a page or paragraph needs
  a glyph the text font lacks, and only while the reader or a Cached UI font is
  active. `releaseLoaded()` (network modes) and BoundedUI loads free it. Its resident
  cost is the font object plus a 107-entry interval table (12 B each); page glyphs
  are prewarmed with the page like the main font.
- Coverage checks use resident interval tables, so a codepoint neither font covers
  costs no SD access however often it repeats. A missing or unreadable fallback file
  is remembered until the font registry is rediscovered. BoundedUI fonts, which keep
  intervals on SD, remember their last eight misses.
- Layout prepares the fallback's advances once per paragraph, and the page prewarm
  loads all fallback glyphs of a page in one set, so grayscale strip passes do not
  reload them through the overflow ring.
- Section (`section.bin` v131) and TXT page caches mix the installed fallback's
  identity into their font key: installing or removing `PocketSymbols` re-lays out
  cached books.

## Layout advance cache

Paragraph layout measures words from a per-style advance cache, not glyph by glyph
from SD (`SdCardFont::buildAdvanceTable`).

- Every advance a paragraph needs is resident when layout starts. Misses are read in
  batches of 256 codepoints, one file open per batch, sorted by glyph index.
- Least recently requested entries are evicted to keep 768 entries (6 KiB) per style.
  One paragraph that needs more (a 750-word flush of dense CJK) may grow its table to
  2,048 entries (16 KiB) until the next page render trims it back.
- Large intervals that one batch misses densely (at least 48 misses, span of at least
  1,024 glyphs) are read once, sequentially, and kept as up to four uniform-advance
  runs. PocketSansWorld's 11,172 Hangul syllables and 20,976 CJK ideographs each have
  a single advance, so after one 178 KB (or 336 KB) scan per style they need no
  further metric reads and no cache entries.
- Host measurement (real parser and layout, PocketSansWorld_12, one chapter with
  1,596 distinct syllables): 10,217 → 3 SD opens, 21,548 → 2 seeks, 21,616 → 419
  reads, layout peak heap 83 → 72 KB (64-bit host), byte-identical section output.
  This is host evidence, not an X3 timing.
