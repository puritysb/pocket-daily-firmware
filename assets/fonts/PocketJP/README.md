# PocketSansJP — Pocket Daily firmware subset

`PocketSansJP_12.cpfont` is the deterministic regular/bold font used by Pocket
Daily's offline Japanese starter deck. It is intentionally a separate family
from the user's complete Japanese reading font, so neither installation can
shadow the other.

- Source: Noto Sans JP variable font, pinned to Google Fonts commit
  `295d98a7a0c17c68f1341eaeea354e7960ea70d3`
- Source SHA-256:
  `c2f3b4d463500a2ddcd3849cded1fceeb9fd6d1c32e6cbecd568453ba50fc68f`
- Raster styles: Medium 500 and Bold 700 at 12 px
- Latin replacement glyph: the repository's Noto Sans regular/bold sources
- License: SIL Open Font License 1.1; see `OFL.txt`
- Derived family name: `PocketSansJP` (the source license reserves `Source`)

The coverage file explicitly includes printable ASCII, U+FFFD, the full-width
parentheses, middle dot, and every device-owned starter-card string. Rebuild and
verify with a Python environment containing `lib/EpdFont/scripts/requirements.txt`:

```sh
python scripts/build-pocket-fonts.py
```
