# Reader resources and SD files

Implemented 2026-09-27; physical acceptance remains pending on updated firmware.

The Reader panel shows RAM free bytes and, when `totalHeap` is present, a used
percentage. RAM is working memory, not a file destination. SD files are separate
from firmware installation. Prepared content displays its exact SD destination;
preparing content does not send it. The Sync file browser lists folders and
reading files, with an explicit permanent-deletion confirmation. App originals
are retained. Hidden/system folders and firmware cannot be deleted here.

## Contract (app and sibling firmware)

Sync `/api/status` advertises `readerFiles: 1` and optional `totalHeap` bytes.
Older firmware remains supported: free RAM only, no invented SD usage.
All endpoints below require the current `deviceID` query and refuse work while
uploads/presentation are busy or the reader has insufficient working memory.
Identity matching prevents accidental cross-device operations; it is not LAN
authentication. Responses use `Cache-Control: no-store`.

- `GET /api/pocket/v1/files?deviceID=…&path=/&cursor=0`: returns `deviceID`,
  `path`, `entries` (name, directory, size bytes, deletable), `nextCursor`.
  At most 12 rows / 48 inspected entries per request. Cursor is a 32-byte aligned
  directory offset, bounded to 8 MiB; zero means finished. Hidden, dot, reserved
  system paths are excluded. This is a live listing, not a snapshot: refresh
  after changes. A folder may contain unsupported files shown as read only.
- `DELETE /api/pocket/v1/files?deviceID=…&path=…&size=…&cursor=0`: only EPUB,
  TXT, MD, XTC files. Size must still match or 409 is returned. Removes associated
  reading cache/recent-book state and article completion marker. Returns
  `deviceID` and `deleted: true`. The app also checks the identity from the
  original listing before submitting confirmation.
- `GET /api/pocket/v1/storage?deviceID=…&cursor=0`: returns `deviceID`,
  `totalBytes`, `freeBytes` for this chunk, `nextCursor`, `supported`.
  FAT16/FAT32 scan up to 4096 clusters per request, using a temporary 512-byte
  sector buffer with watchdog yields. The app sums chunks, validates fixed
  capacity and cursor progression, and supports cancellation between requests.
  Other formats expose capacity only (`supported: false`). Usage is a refreshed
  estimate; external changes during the scan require refreshing again.

No recursive delete, formatting, moving system files, or RAM editing is exposed.
File browsing and SD usage require firmware containing these endpoints; this
change has not yet been installed on the physical X3. Directory enumeration uses
SdFat's entry iterator: exceptionally sparse/damaged directories still require
hardware timing validation. Reboot/SD removal during a delete may leave cleanup
incomplete; refresh the folder to establish whether the file remains.

## Reader file download (agreed and implemented 2026-10-01; hardware pending)

Purpose: a book that exists only on the reader can be added to the app Library
on an explicit user action ("Add to Library"). Never automatic or in the
background. Agreed between the firmware and app sessions. Firmware:
`handleReaderFileContent` (`src/pocket_daily/web/PocketEndpoints.cpp`) with the
rules in `ReaderFilesPolicy.h` (`downloadableReaderFile`, `parseByteCount`,
`planDownloadPiece`, `downloadPieceLimit`, `readDownloadPiece`); host tests in
`test/hal_storage` (`ReaderFilesPolicy.*`, including a reassembly test over both
piece limits that fails if a piece is read from the wrong offset) and the
route guard in `scripts/test_sync_routes.py`. The HTTP layer itself (status
codes, headers) is only checked on a device. Not yet run on X3/X4.
The app accepted it unchanged (2026-10-01): it gates listing/delete/storage on
`readerFiles >= 1` and download on `readerFiles >= 2`, restarts from offset 0
at most once on `409`, and tests pieces, backoff, restart, `416` and size
mismatch against a fake server.

Matching first, without new firmware: `GET /api/pocket/v1/reading` (recent
EPUBs with partial-MD5 `document`) gives exact matches; `GET /files` name+size
matches files the app uploaded unchanged. Per-entry `document` in the listing is
**not** added: the id is computed by reading the file
(`KOReaderDocumentId::calculate`) and there is no per-file cache outside the
recent books, so the listing stays a cheap directory walk.

- Advertised as `readerFiles: 2` in Sync `/api/status` (both Sync profiles:
  Same Wi-Fi and Direct). `readerFiles: 1` firmware (including
  `pocket-v1.0.0-beta.1`) has list/delete/storage only; the app gates download
  on `readerFiles >= 2`.
- `GET /api/pocket/v1/files/content?deviceID=…&path=…&size=…&offset=N`
  returns **one piece** of the file starting at `offset`: `200`,
  `Content-Type: application/octet-stream`, exact `Content-Length` (never
  chunked), `Cache-Control: no-store`. The reader chooses the piece size:
  at most 4096 bytes on Same Wi-Fi and 1024 bytes on Direct, from the shared
  staging buffer with one bounded socket write (the crash-report pattern). The
  app advances `offset` by the bytes received and must not assume a fixed size.
  No `Range` header and no whole-file stream: on the X3, a single long HTTP
  readback stopped after 8192 of 16384 bytes (`docs/TRANSFER_BENCHMARK.md`),
  while bounded pieces are the proven path; an interrupted download resumes
  from the last offset by design.
- Only `.epub`, `.txt`, `.md` (case-insensitive) outside hidden, dot and
  reserved system paths, the same rules as the listing. XTC, firmware and
  everything else: `403`.
- `400` malformed query (missing/non-decimal `offset` or `size`), `404` missing
  path or a directory, `409` `deviceID` mismatch, size mismatch (the file
  changed: restart from 0) or busy (upload stream, presentation), `416`
  `offset >= size`, `503` below the diagnostics heap floor (10 KiB free); back
  off (0.5 s doubling to 4 s) and retry the same offset. Expect frequent `503`
  on an X3 Direct session; prefer Same Wi-Fi in the UI.
- Integrity: the app requires `sum(pieces) == size`; for EPUBs it may compare
  its own partial MD5 with `/reading`'s `document` when present. The reader
  does not hash whole files. The size check cannot detect wrong content
  of the right length: an EPUB is caught by its ZIP CRCs when the app imports
  it, a TXT/MD file is not, so byte-identity on a device (SHA-256 of 1 MB and
  20 MB files, both bearers) is the acceptance test for this route.
- One download at a time; each piece counts as client activity for the session
  timeout. No firmware size limit beyond the SD file system; the app should
  confirm unusually large files. Throughput is unmeasured: one request per
  piece, X3 request turnaround observed 0.03–0.7 s, so a 1 MB EPUB is a minute
  or more. Show progress and allow cancel.
- Verification when implemented: host tests for path/type/offset/size/identity
  rules and piece bounds; on X3/X4 Same Wi-Fi and Direct, a 1 MB EPUB and a
  20 MB EPUB downloaded byte-identical (SHA-256), with heap and watchdog logs.
