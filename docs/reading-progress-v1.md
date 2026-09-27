# Reading progress v1 — serverless positions between app and reader

Implemented 2026-09-27; host-verified only. Not installed on X3/X4. The app-side
contract is `docs/READING_PROGRESS.md` in the sibling `pocket-daily` repository
("리더 직접 교환"); this file is the firmware original and must stay in step.

Continue-reading between the Pocket Daily app and the reader over the existing
Sync connection (LAN or private AP), with no KOReader server. Positions use the
KOReader form: a partial-MD5 document digest, an XPointer, and a start-of-page
percentage. Neither side moves the other's page: a reader position is only
offered in the app, and an app position is only asked about on the reader.

## Capability

Sync profiles (`POCKET_SYNC`, `COMPANION`) advertise `"readingProgress": 1` in
`/api/status` and register both routes below; other profiles do neither. Older
firmware omits the key and the app must not call the routes.

## `GET /api/pocket/v1/reading?deviceID=<id>`

```json
{"v":1,"deviceID":"<id>","books":[{"path":"/Books/a.epub",
 "document":"<32 hex>","filenameDocument":"<32 hex>",
 "progress":"/body/DocFragment[3]/body/section[1]/p[12]/text()[1].96",
 "percentage":0.43000,"updated":1790000000,"seq":17}]}
```

- Up to 10 EPUBs from Recent Books, most recent first. Missing files, non-EPUB
  entries and entries that would not serialize are skipped. The whole body is
  at most 8 KiB (`ReaderReadingList.maximumBytes`); older books are dropped
  first. Sent with chunked transfer encoding and `Cache-Control: no-store`.
- `document`: KOReader partial MD5 (`KOReaderDocumentId::calculate`), lower-case.
  Cached in the book's record with the file size it was computed for; recomputed
  (12 × 1 KiB reads, nothing written) when the size differs or no record exists.
- `filenameDocument`: MD5 of the file name (KOReader filename mode); `""` if it
  cannot be computed.
- `progress`: the XPointer the reader computed when the book was last left, or
  `null` when there is none or when the saved page (`progress.bin`) no longer
  matches the record (for example after power loss). Never computed here.
- `percentage`: 0–1 at the start of the page (`Epub::calculateProgress(spine,
  page / pageCount)`); 1 on the end-of-book screen. Without a matching record it
  falls back to the whole-percent byte of `progress.bin`, else 0.
- `updated`: epoch seconds when the record was written if the clock was set
  (≥ 2023-11-14, NTP or app glance), else 0. 0 whenever `progress` is null.
- `seq`: reader-wide counter, advanced per new record. Informational: a lost or
  damaged counter restarts at 1; do not order positions by it.
- Errors: 409 reader identity mismatch or transfer active (same admission as the
  other Pocket routes), 503 when free heap < 12 KiB or largest block < 4 KiB.

## `POST /api/pocket/v1/reading`

Body (≤ 1 KiB, JSON; `deviceID` is in the body, not the query):
`{"deviceID":"<id>","document":"<32 hex>","progress":"<xpointer>","percentage":0.5,"device":"Pocket Daily iPhone"}`

- Validation, all-or-nothing: `deviceID` 8 characters; `document` 32 hex
  (upper case accepted, compared lower-case); `progress` printable ASCII
  starting `/body/DocFragment[`, ≤ 512 bytes; `percentage` a number in 0–1;
  `device` non-empty strict UTF-8 without control characters, ≤ 64 bytes.
  Unknown keys are ignored. Malformed → 400 (503 if the parser ran out of heap).
- Identity is checked after parsing: mismatch or active transfer → 409.
- The book is found among Recent Books by `document` or `filenameDocument`.
  None → 404 (the app treats 404 as "nothing to do").
- Stores a pending offer in the book's cache directory, replacing an earlier
  one, and replies `{"ok":true,"state":"pending"}`. The current position,
  `progress.bin` and the record are never changed. Storage failure → 500.

## On the reader

When the book is next opened and its first page is laid out, a pending offer
more than 0.004 (0.4 %) beyond the current page start opens a confirmation:
`"<device>: <pct>% · Go there?"` (`STR_READING_OFFER_FORMAT`, English and
Korean; other languages fall back to English). Confirm (Right) moves there
through `ProgressMapper::toCrossPoint`; Cancel (Left) stays. The offer is
deleted after either answer. An offer that is not further, or is damaged, is
deleted silently. If the reader sleeps or exits during the question, the offer
remains and is asked again on the next open.

When a book is left (`EpubReaderActivity::onExit`, which also runs before deep
sleep), the reader records the page:

1. While the Section is loaded it reads its paragraph LUT around the page once
   (`Section::getParagraphRunForPage`): the previous page's entry is the `<p>` the
   page starts in, and the run of pages ending inside that paragraph gives the
   fraction into it (`(page − first − 0.5) / (last − first + 1)`, i.e. text spread
   evenly with half-filled first and last pages).
2. The Section is released, then the XPointer is resolved
   (`ChapterXPathResolver::findXPathForParagraphProgress`, two streams of the
   chapter; fallback `findXPathForProgress(page / pageCount)`). Page 0 is the
   chapter start `/body/DocFragment[N]/body`. Nothing is written when the page
   is unchanged.
3. The chapter streams from the reader's inflated copy
   `<cache>/html/<spine>.html` when present (1 KiB buffer + expat), otherwise
   from the ZIP, which needs the 32 KiB inflate window. The XPointer is skipped
   (reported as `null`) unless the largest free block is ≥ 12 KiB (cached) or
   ≥ 40 KiB (ZIP). The log line `RPS Recorded … in N ms (heap, block)` is the
   hardware evidence for cost and heap.

Precision: character-accurate inside a paragraph when the LUT exists, estimated
proportionally for pages inside a long paragraph; a building (partial) section
without a LUT falls back to a fraction of the chapter's paragraph text.

## XPointer rules (shared with the app's `xpointer.js`)

- `/body/DocFragment[N]` is the 1-based spine index; element steps count
  same-name siblings. The firmware writes every index (`p[1]`, `text()[1]`);
  it reads the app's form that omits `[1]`, and element-only paths.
- `text()[N]` counts only the element's direct text runs (split by its child
  elements) that contain a non-whitespace character (ECMAScript `\s` class,
  including U+00A0, U+2000–200A, U+3000, U+FEFF). Text inside child elements
  belongs to those children.
- The offset counts Unicode code points from the start of that run (leading
  whitespace included) after XML line-end normalization (CRLF/CR → LF) and
  entity decoding: each of the five XML entities (`&apos;` included), a numeric
  reference, or an HTML named entity known to `lib/Epub/Epub/htmlEntities`
  (`&nbsp;`, `&hellip;`, …) is one decoded character, as in the app's DOM.
- An offset past the run's end clamps to it; a missing text node lands on the
  element's start; a missing element is unresolved (percentage fallback).
- KOReader keeps some whitespace-only inline text nodes this rule skips (for
  example the space in `<i>a</i> <i>b</i> c`); such KOReader positions land on
  the element instead of the exact character.

## Files (per book, in `.crosspoint/epub_<hash>/`)

Little-endian, CRC-32 over everything before the trailing CRC; decoders reject
any bad field. Moving or clearing a book's cache takes them along.

- `pocket-reading.bin` (`"PDRP"`, version 1): reserved u8, spine u16, page u16,
  page count u16, percentage f32, seq u32, updated u32, file size u32, digest
  length u8 (0 or 32) + digest, XPointer length u16 (≤ 512) + XPointer, CRC u32.
  Written to `.tmp` and renamed.
- `pocket-reading-offer.bin` (`"PDRO"`, version 1): reserved u8, percentage f32,
  XPointer length u16 + XPointer, device length u8 (≤ 64) + device, CRC u32.
- `/.crosspoint/pocket-reading-seq.bin`: u32 value + u32 bitwise complement.

## Verification

Host (`test/reading_progress`):

- `reading_progress_xpointer_test` resolves every app XPointer with the
  firmware's `ProgressMapper::locateInSpine` (the step `toCrossPoint` uses) and
  compares the visible text there with the app's `textAt`: Korean sample 15/15,
  Standard Ebooks Frankenstein 114/114 exact. Fixtures:
  `fixtures/app-xpointers-*.json`, EPUBs rebuilt by `fixtures/make_fixtures.py`.
- The same test generates firmware XPointers (paragraph + fraction and chapter
  fraction) for every spine item of both books, round-trips them, and writes
  `fixtures/firmware-xpointers.json` (258 positions with `textAt`) for the app
  to resolve with its own engine. `READING_PROGRESS_UPDATE_GOLDEN=1` rewrites it;
  a change fails the test until the app re-verifies it.
- `reading_progress_format_test`: record/offer codecs, body validation limits,
  list JSON and budget, SD store and sequence counter (fake SD).
- `scripts/test_sync_routes.py`: routes in the Sync block, no chapter streaming
  or position writes in the handlers, exit-hook ordering.

Needs X3/X4: exit latency and heap of the XPointer computation on a large
chapter (ZIP and cached paths), GET/POST over LAN and private AP with the app,
the confirmation on both button layouts and four orientations, the jump landing
on the offered page, and that sleep entry stays prompt.
