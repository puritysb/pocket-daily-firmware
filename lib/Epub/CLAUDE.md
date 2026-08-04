# lib/Epub — cache formats and invalidation

The reader's speed comes from an aggressive SD-side cache. Its correctness rests entirely on
the version byte in each file header: get that wrong and the reader silently deserialises a
stale layout instead of failing loudly.

## Cache structure on SD

**Location**: `.crosspoint/` at the SD card root.

**Layout**: `.crosspoint/epub_<hash>/{book.bin, progress.bin, cover.bmp, sections/*.bin}`

**Hash**: `std::hash<std::string>{}(filepath)` — so moving or renaming a book yields a new
hash, a new cache, and **lost reading progress**. That is expected behaviour, not a bug.

## File format versioning

**Source**: `lib/Epub/Epub/Section.cpp`, `lib/Epub/Epub/BookMetadataCache.cpp`

| File | Version |
|---|---|
| `book.bin` | **7** (metadata structure) |
| `section.bin` — upstream baseline | **27** (layout structure) |
| `section.bin` — **this fork** | **129** |

The fork adds a `bilingualViewMode` header field and numbers itself in the **reserved 128–255
range** rather than `upstream + 1`. Upstream keeps incrementing 27→28→…; a
same-numbered-but-different-layout header would defeat the `version != SECTION_FILE_VERSION`
check and feed stale caches to the reader. See the `SECTION_FILE_VERSION` comment in
`Section.cpp`.

v129 also stores a `BILINGUAL_MODE_ANY` sentinel for chapters with no bilingual markers, so
cycling view modes reuses their cache instead of forcing a full re-parse
(`Section::bilingualModeAgnostic`).

**Rules**:
1. **ALWAYS increment the version BEFORE changing binary structure.** Not after, not in the
   same commit as an afterthought — the mismatch is the only thing protecting the reader.
2. Version mismatch → cache auto-invalidated and regenerated. That is the intended mechanism.
3. Document format changes in `docs/file-formats.md`.
4. **Fork rule**: keep the fork's `SECTION_FILE_VERSION` in the **128+ range** (bump within
   it), never onto upstream's line. The upstream-contribution branch
   (`bilingual-toggle-upstream`) uses `upstream + 1` instead. See the `fork-sync` skill.

```cpp
// lib/Epub/Epub/Section.cpp — fork line
static constexpr uint8_t SECTION_FILE_VERSION = 130;  // was 129; bump within 128–255

struct PageLine {
  // ... existing fields ...
  uint16_t newField;  // new field added — hence the bump above
};
```

## What else invalidates a cache

1. **Format version changes** (above)
2. **Render settings change** — `SETTINGS.fontFamily`, `fontSize`, `lineSpacing`,
   `extraParagraphSpacing`, `screenMargin`
3. **Viewport dimensions change** — orientation or display resolution
4. **Book file modified** — moved, renamed, or content changed (new hash)

## Clearing by hand

```bash
rm -rf /path/to/sd/.crosspoint/                          # all caches
rm -rf /path/to/sd/.crosspoint/epub_<hash>/              # one book
rm -rf /path/to/sd/.crosspoint/epub_<hash>/sections/     # keep progress, drop layout
```

Clear the cache when: EPUB parsing breaks after changes under `lib/Epub/`; rendering is
corrupt (missing text, wrong layout); you are testing cache generation itself; or you touched
`Section.cpp`, `BookMetadataCache.cpp`, or render settings in `CrossPointSettings`.

**Flag this for the user** — you cannot clear an SD card they have not mounted. If a change
here needs a cache wipe to test, say so explicitly rather than assuming a clean re-parse.
