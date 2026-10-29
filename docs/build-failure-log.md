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

The key is also absent in the Sync profile, below the diagnostic heap gate, or
when the record cannot be read or validated; absence does not prove that no
failure occurred. `freeHeap`/`largestBlock` are read after the failed build
returns, before the section is released. ZIP input/output buffers and inflater
state have already been freed at this point: these are **post-cleanup** values,
not the available heap at the failing allocation.

| step | where the build stopped | detail |
|---|---|---|
| `html-stream` | inflating the chapter into `html/<spine>.html` | `ZipFile::StreamError`: `open`, `entry`, `read-buffer`, `output-buffer`, `window` (inflater state or contiguous window allocation), `inflate`, `read`, `write`, `method` |
| `section-file` | opening `sections/<spine>.bin.part` | — |
| `build-context`, `parser` | allocating build state or the chapter parser | — |
| `begin-parse` | opening the HTML or starting expat | — |
| `layout` | parse or layout error while building pages | `out-of-memory` when layout ran out of heap |
| `commit` | writing the tables or moving the section file into place | — |

The record is diagnostic only and does not control reading. The file is
downloadable through File Transfer. Developer firmware additionally provides
the bounded diagnostic route described in [developer-pipeline.md](developer-pipeline.md);
production Sync does not expose this file. Internal retries do not increment
the record count: only an ultimately failed section build does. A successful
retry leaves the previous failure record unchanged.

## Current inflater and bounded retry (2026-10-25)

The EPUB stream uses `ZipFile` and `lib/miniz/src/InflateStream.cpp`. For entries at least
as large as the output chunk, the normal chapter path allocates an 8 KiB input
buffer, an 8 KiB output buffer, a miniz state of about 8 KiB and one contiguous
32 KiB window. The four 8 KiB segments in
`lib/InflateReader` belong to a separate inflater used by fonts; they are not the
EPUB streaming path.

An X3 record from production 0.1.0 confirmed `html-stream` / `window` on spine 1
of the original English acceptance fixture. The precise page is unknown.
Its 86,920 free bytes and 49,140-byte largest block were measured after cleanup;
freed buffers can coalesce into a block larger than was available during init.
The `window` detail covers both state and window initialization failures and
does not identify which allocation failed by itself.

Section keeps its existing maximum of three attempts. After `Window` only, the
next attempt uses 1 KiB input/output buffers, reducing their transient footprint
by 14 KiB. The initial attempt and retries after other errors retain 8 KiB
buffers. Every attempt starts a fresh inflater and truncates its temporary HTML;
only a complete successful inflate may be renamed into the HTML cache. A temp
file open failure reports `write`, even if an earlier attempt failed at `window`.
Large-entry decoding and its 32 KiB window are unchanged. The later semantic
section-cache invalidation is described below.

Host fault tests exercise the actual Section/ZIP/miniz pipeline, including
bounded repeated failure, allocation cleanup and byte-identical HTML/section
output after recovery. They are not an ESP32 allocator or SD-card simulation:
the device still needs a contiguous window and physical validation is required.


## Small ZIP entries and image header probes

A later X3 development walk confirmed display mode `0` with one image header
probe and its fallback extraction both failing at inflater initialization
(`Window`), followed by an ALT label. The pre-probe heap was 55,608 bytes free
with a 40,948-byte largest block. These are before the ZIP input/output/state
allocations, not the block available when the ring is allocated. The original
PNG was a 409-byte deflated entry.

`ZipFile::readFileToStream` now uses the existing output chunk as a complete
non-wrapping destination when the declared uncompressed entry is **strictly
smaller** than that chunk. No new allocation is added; the transient miniz state
remains, while the separate 32 KiB ring is unnecessary. This covers any small
entry, including a PNG probed with 1024-byte chunks, extracted with 4096-byte
chunks or lazily extracted with 8192-byte chunks. Exact-boundary and larger
entries keep the existing streaming path.

The strict inequality leaves space to observe the deflate end marker in a
single call. A small entry must return `Done` with exactly its declared size
before any bytes reach the sink; its buffer start is never reused for a second
inflate call. Valid short writes still succeed only with explicit
`allowEarlyStop`. Malformed small entries now fail before a header sink can turn
a partial result into success. This is an intentional tightening of malformed
input handling; large-entry early-stop behavior is unchanged.

The section byte layout is unchanged, but its semantic version is now136.
Completed v135 sections and partial trailers tagged135 are rejected and rebuilt
when normally opened, so previously cached ALT output is not retained forever.
Only the affected section file and its temporary file are removed at that time;
retained HTML, metadata, progress and reading-offer records remain. The reader
restores by saved visible-text offset when available, otherwise by its existing
chapter page-ratio fallback; page numbers can change when an image is restored.
Physical verification first uses a fresh owned fixture path and does not manually
delete original caches. This change does not establish that
large images fit the available heap or that the PNG decoder can allocate and
paint successfully. The physical run must confirm mode, image diagnostics and
actual pixels after installing the candidate.
