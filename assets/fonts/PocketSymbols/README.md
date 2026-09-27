# PocketSymbols

`PocketSymbols_12.cpfont` is the reader's glyph-fallback font. When the reading
font (for example PocketSansWorld or a built-in font) lacks a visible glyph, the
firmware borrows it from this font: monochrome emoji, dingbats, arrows,
geometric shapes, letterlike, technical and mathematical symbols. It is never
offered as a reader font.

Install path on the SD card (discovered at boot; `/fonts/` also works):

    /.fonts/PocketSymbols/PocketSymbols_12.cpfont

## Provenance

All sources are pinned to Google Fonts commit
`23e54b51ddffbc7713c583748e3bd86f62b1fa4a` (`ofl/` directory):

| Source | File | SHA-256 | Version | License |
|---|---|---|---|---|
| Noto Emoji (monochrome, variable wght 300–700) | `notoemoji/NotoEmoji[wght].ttf` | `de6c18832938afc99caf132b39d6a30a19bac7f2e812e28db2535b4608d27551` | 3.002 | OFL 1.1, `NotoEmoji-OFL.txt` |
| Noto Sans Symbols 2 | `notosanssymbols2/NotoSansSymbols2-Regular.ttf` | `7d5fb73b7ca67a6798101741f5d280a3d016a56a197afcd4199dbb57b4b82a21` | 2.008 | OFL 1.1, `NotoSansSymbols2-OFL.txt` |
| Noto Sans Math | `notosansmath/NotoSansMath-Regular.ttf` | `3f495fe933c06786e4d5f6d86b8ee70b6753a68ee3b9d87528726de0f6e2c47d` | 3.000 | OFL 1.1, `NotoSansMath-OFL.txt` |

Each license file is the unmodified `OFL.txt` shipped beside its source in the
Google Fonts repository. None declares a Reserved Font Name (the build script
refuses to run if one appears); the derived family is nevertheless named
`PocketSymbols`, not Noto. Apple Color Emoji, Segoe UI Emoji and Twemoji
artwork are not used.

## Build

- Noto Emoji is instanced at weight 500 (matching the Medium reading weight)
  and its em is enlarged by 1.2 so emoji match a Hangul syllable or ideograph
  at the same size; the other sources are used unchanged.
- Lookup priority per codepoint: Noto Emoji, then Symbols 2, then Math.
- Subset: every codepoint those fonts cover inside U+00A9, U+00AE, U+203C,
  U+2049, U+2100–214F, U+2190–23FF, U+2460–24FF, U+25A0–27FF, U+2900–2BFF,
  U+3030, U+303D, U+3297, U+3299, U+1F000–1F2FF, U+1F300–1F6FF,
  U+1F780–1F8FF and U+1F900–1FAFF, minus codepoints the firmware never draws
  (Default_Ignorable_Code_Point, emoji skin-tone modifiers). The exact list is
  `coverage.txt`. Braille and mathematical alphanumerics are left out.
- Result: one regular style, 3,888 glyphs in 107 intervals, 413,062 bytes,
  12 px (150 DPI), 2-bit grayscale. SHA-256
  `8c2481b50661c532daf4de528504a163ffc6ca903fe7cc2e76afc1fae009d138`
  (fontTools 4.66.0, freetype-py 2.5.1).

Rebuild and verify coverage with a Python environment containing
`lib/EpdFont/scripts/requirements.txt`:

```sh
python scripts/build-pocket-symbols.py
```

Resident cost on the reader when loaded: the interval table (107 × 12 B) plus
the font object; glyphs load per page like any SD font. See
`docs/sd-card-fonts.md`.
