# Section build failure log

When the reader cannot build an EPUB chapter it shows "Failed to index - invalid
book or SD error". Without a USB console the cause used to be unrecoverable.
The reader now keeps the last failure in one small SD file and reports it over
Wi-Fi.

## Record

`/.crosspoint/last-build-error.bin`, written by `EpubReaderActivity` only when a
section build fails (`src/pocket_daily/BuildFailureLog.*`). A 96-byte
little-endian record: magic `PDBF`, format 1, step, detail, spine, repeat count,
book, free heap, largest free block, uptime (s), firmware version (64 bytes,
NUL-terminated), CRC-32. Written to a `.tmp` file and renamed, so a torn write
leaves the previous record. A failure of the same book and spine increments the
count; another chapter starts again at 1. It contains no book text or path:
`book` is the hash that names the book's cache directory
(`/.crosspoint/epub_<book>`).

## `/api/status`

When SD diagnostics are affordable (the gate used for `crashReportAvailable`:
not the Sync heartbeat profile, free heap at least 10 KiB), status carries:

```json
"lastBuildError": {
  "book": 464604871, "spine": 5, "step": "html-stream", "detail": "window",
  "count": 3, "freeHeap": 61234, "largestBlock": 28672, "uptime": 812,
  "version": "1.7.0-dev-..."
}
```

The key is absent when nothing was recorded. `freeHeap`/`largestBlock` are read
right after the failure, before the section is released.

| step | where the build stopped | detail |
|---|---|---|
| `html-stream` | inflating the chapter into `html/<spine>.html` | `ZipFile::StreamError`: `open`, `entry`, `read-buffer`, `output-buffer`, `window` (deflate window segments), `inflate`, `read`, `write`, `method` |
| `section-file` | opening `sections/<spine>.bin.part` | — |
| `build-context`, `parser` | allocating build state or the chapter parser | — |
| `begin-parse` | opening the HTML or starting expat | — |
| `layout` | parse or layout error while building pages | `out-of-memory` when layout ran out of heap |
| `commit` | writing the tables or moving the section file into place | — |

The record is informational: nothing reads it back on the reader, and deleting
it is harmless. The file is also downloadable through File Transfer.

## Background (2026-09-27)

The first recorded case, an X3 reading a bilingual novel, failed at
`html-stream` before this log existed: the chapter's inflated HTML and section
file were never created. The streaming inflater then needed one contiguous
32 KiB window; it now uses four 8 KiB segments (`lib/InflateReader`).
