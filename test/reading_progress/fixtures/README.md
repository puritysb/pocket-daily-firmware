# Reading-progress cross-check fixtures

- `app-xpointers-korean.json`, `app-xpointers-frankenstein.json`: positions the
  Pocket Daily app's reader engine (`Support/ReaderEngine/xpointer.js`) produced,
  each with the next source characters (`textAt`).
- `pocket-daily-epub-check.epub`: original app-generated sample (Korean, emoji,
  one 30k-character paragraph), re-zipped with deflate.
- `frankenstein-se.epub`: Standard Ebooks "Frankenstein" (text public domain,
  markup CC0 1.0, https://standardebooks.org/ebooks/mary-shelley/frankenstein),
  images replaced by 1×1 placeholders. Spine XHTML is unchanged.
- `firmware-xpointers.json`: positions the firmware generates, for the app to
  resolve (`READING_PROGRESS_UPDATE_GOLDEN=1` regenerates it).

`make_fixtures.py` rebuilds both EPUBs from the originals.
