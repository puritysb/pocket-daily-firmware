# Pocket content card v1 (PDCT)

Default cards retain the exact v1 bytes below. Alternate layout presets use the
explicit v2 extension in [content-card-layout-v2.md](content-card-layout-v2.md),
with capability negotiation; reserved v1 bytes are never reinterpreted silently.

Status: codec implemented in firmware `ContentCard.cpp` and companion
`Sources/Studio/ContentCard.swift`, connected to local revision activation and
shared device/host rendering. **Physical acceptance remains unverified**;
historical incremental notes below are not the current implementation status.
This is the text-card payload referenced by a PDCM manifest entry of kind 1.
It is not the provider's wire JSON and does not change `.pdl` or `.uipack`.

## Exact 512-byte layout

All integers are little-endian. Strings are fixed-size, NUL-terminated and
zero-padded. Padding/reserved bytes must be zero; truncating text is forbidden.

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 4 | ASCII `PDCT` |
| 4 | 2 | Version 1 |
| 6 | 2 | Header bytes 16 |
| 8 | 4 | Total bytes 512 |
| 12 | 4 | Reserved flags, zero |
| 16 | 33 | ID: 1–32 ASCII `[a-z0-9_-]` bytes |
| 49 | 25 | Required UTF-8 title, at most 24 bytes |
| 74 | 161 | Required UTF-8 primary text, at most 160 bytes |
| 235 | 192 | Optional UTF-8 secondary text, at most 191 bytes |
| 427 | 64 | Optional image filename, PDCM `.pbm` filename rules |
| 491 | 17 | Reserved, zero |
| 508 | 4 | CRC-32/ISO-HDLC of bytes 0–507 |

CRC uses reflected polynomial `0xEDB88320`, initial/final XOR `0xFFFFFFFF`.
The manifest also carries the SHA-256 of the entire 512-byte file.
The fixed size and field capacities reuse the existing `PocketDaily::Card`
storage/render limits without increasing the permanent card pool or allocating
a second pack. These are **UTF-8 byte** limits, not character limits: a title
can hold eight three-byte Hangul syllables. Longer titles need a separately
versioned future renderer/model change, not silent truncation.

UTF-8 validation rejects overlong sequences, invalid continuations, truncation,
surrogates, values above U+10FFFF and controls U+0000–001F/U+007F–009F. LF is
allowed only in primary/secondary text. CR, tabs and embedded NUL are rejected.
Unicode normalization is not performed; distinct valid bytes remain distinct.

## Mapping and integration boundary

The decoder writes into caller-owned `ContentCard` storage using reads of at
most 192 bytes, with no allocation. Every failure clears the entire output.
Successful output maps to existing `PocketDaily::Card` fields:

- `cardId`: `app:` plus the file ID.
- `module`: `app` (not the built-in study deck's `local`).
- `actionClass`: `info`.
- `title`, `question`, `context`: the three text fields.
- `choiceCount`: zero; all choices remain empty.
- Image filename stays in the surrounding `ContentCard.imagePath` field.

There are no script, URL, filesystem command, provider action or arbitrary
choice fields. The prefix is a namespace convention, not authentication.
**Do not append decoded cards directly to the provider state pool.** Activation
must validate IDs/references across the revision and install an app-owned pool;
the UI integration must explicitly handle app-card view/dismiss locally, without
sending provider outbox actions. Existing generic handlers are not yet adapted.
The existing card renderer does not yet display `imagePath`.

An image filename is only syntactically validated by the card decoder. The
[PBM validator and app revision builder](content-image-v1.md) are now available.
Activation must still verify
that it names a kind-2 file in the same revision, then verify its exact bytes,
SHA-256 and supported bitmap semantics before committing. Missing images must
not be silently accepted. No capability advertisement or upload route is added
by this codec, and no device installation is needed to run its host tests.

## Shared golden

Fields: ID `morning-1`, title `오늘`, primary text `한 줄 읽기\nRead one line.`
(one literal LF), secondary text `가볍게 시작하세요.`, image `sun.pbm`.
All other bytes are zero except the fixed header and checksum.

- CRC: `56c2918d` (stored `8d 91 c2 56`).
- Entire-file SHA-256:
  `dc6d7e86f5ac0a6114fc6f49ea62572874a355e0a250e01b663508a805d6e1dc`.
- Tests: firmware `test/content_manifest/ContentCardTest.cpp`, companion
  `Tests/ContentCardTests.swift`.

The fixture references an image but does not include it; it proves card codec
agreement, not completeness or activation of a revision.
