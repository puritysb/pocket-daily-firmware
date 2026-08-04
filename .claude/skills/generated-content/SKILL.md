---
name: generated-content
description: Changing content that a build script generates rather than hand-written source. Use when editing UI text or translations, adding a language, touching the web/config HTML pages, adding or converting a font, or when you find yourself about to edit a *.generated.h, I18nKeys.h, I18nStrings.h/.cpp file. Covers which file is the real source, the regeneration command, and what to commit.
---

# Generated Content

Three asset families in this repo are **generated from source at build time**. Editing the
output is always wrong: the next `pio run` silently overwrites it, and the change is gone
without an error. CLAUDE.md lists which files are generated; this is how to change each one.

The invariant: **find the source, edit that, regenerate, commit the source only.**

## UI text and translations (i18n)

Source: `lib/I18n/translations/<language>.yaml` — one file per language.
Generated: `lib/I18n/{I18nKeys.h, I18nStrings.h, I18nStrings.cpp}` (all three gitignored).

1. Edit or add the YAML file. Each must contain `_language_name`, `_language_code`, `_order`,
   and the `STR_*` keys.
2. `english.yaml` is the reference. Missing keys in other languages fall back to English —
   so a partial translation is valid, not broken.
3. Regenerate:
   ```bash
   python scripts/gen_i18n.py lib/I18n/translations lib/I18n/
   ```
4. **Commit the YAML only.** All three generated files are gitignored and rebuilt at build time.

Using a string in code — never hardcode user-facing text:
```cpp
#include <I18n.h>
renderer.drawText(FONT_UI, x, y, tr(STR_LOADING), true);  // StrId enum from generated I18nKeys.h
```
Log messages (`LOG_DBG` / `LOG_ERR`) are exempt — they are not user-facing.

## HTML pages

Source: `data/html/<pagename>.html`
Generated: `src/network/html/<pagename>Html.generated.h`, via `scripts/build_html.py` in the
PlatformIO `pre:` build step.

1. Edit the HTML under `data/html/`.
2. `pio run` — the header regenerates automatically, no separate command.
3. **Commit the HTML only**, never the `.generated.h`.

Remember these pages are served off a device with ~380KB RAM: page weight is a memory cost,
not just a download.

## Fonts

1. Place source fonts in `lib/EpdFont/fontsrc/` (gitignored).
2. Run the conversion script — see `lib/EpdFont/README`.
3. Register the global font object in `src/main.cpp` (inside the `#ifndef OMIT_FONTS` guard).
4. Add the font ID constant to `src/fontIds.h`.

Each font is a global static costing flash for glyphs plus **DRAM once rendered**. Adding one
is a memory decision — justify it, and check the `scope-discipline` skill if it is for a new
feature.

## Before handing off

- `git status` — is any generated path staged? Cross-check against `.gitignore`
  (`*.generated.h`, `I18nKeys.h`, `I18nStrings.*`, `.pio/`, `compile_commands.json`).
- Added an i18n key? Regenerate before building, or the build fails on a missing `StrId`.
- Added a language? Confirm `_order` does not collide with an existing one.
- `pio run` clean.
