# Content read v1 (loading the reader's cards into the companion)

Status: **implemented and host-tested (2026-09-25); not yet exercised on a
reader.** Companion: `Sources/Studio/ReaderContentPull.swift` in the app.

The companion can bring the cards a reader shows back into its editor, for
example after the local draft was deleted or edited on another device. The
reader serves the files of a published revision read-only; the companion
verifies every byte and opens the result in the same side-by-side review as a
draft-file import. Nothing on the reader changes.

## Endpoint

`GET /api/pocket/v1/content/file?deviceID=<8hex>&revision=<64lowerhex>&name=<leaf>&offset=<n>`

- Registered with the other content routes on every profile; `/api/status`
  advertises `"contentRead": 1`.
- Admission is the content-operation gate (reader identity, 8 KiB free /
  2 KiB largest block, no HTTP or stream upload receiving) and additionally
  refuses while a stream transfer is active, because the response is read
  into the shared 4 KiB staging buffer that stream uploads batch into.
- `revision` must be 64 lowercase hex characters. `name` must be
  `manifest.pdcm` or a manifest-shaped `.card` / `.pbm` leaf name
  (`Content::publishedFilePath`); the path is always
  `/pocket-daily/content/<revision>/<name>`. Anything else is 400.
- `offset` is 1–7 decimal digits. The reply is up to 4,096 bytes of the file
  from that offset as `application/octet-stream`, sent with the same bounded
  socket timeout as the screen preview. 404 when the file does not exist,
  416 when the offset is at or past its end.
- Any published revision can be read, not only the active one: published
  revisions are immutable and content-addressed. The companion asks
  `GET /api/pocket/v1/content/state` for the active revision first.

## Companion verification

1. `content/state`; no active revision means the reader has no app cards.
2. Read `manifest.pdcm` from offset 0 until the size its header declares (at
   most 1,684 bytes). Its SHA-256 must equal the revision, and it must decode
   to exactly the bytes the companion's encoder would write.
3. Read every listed file to its manifest size; each SHA-256 must match the
   manifest. Cards must decode to canonical PDCT bytes (v1 or v2 layout) and
   images must be valid PBM.
4. Cards keep manifest order, which is the companion's editor order. The draft
   is shown for review; Replace changes only the editor. Sending the pulled
   draft reproduces the same revision.

Any short, long, empty, altered or unsupported read fails without a draft.

## Tests

- Firmware: `ContentPublishedFilePath` host test (path confinement);
  `scripts/test_sync_routes.py` source check (route, admission, no writes,
  status flag).
- Companion: `Tests/ReaderContentPullTests.swift` (manifest/card decoders
  against the shared goldens, chunked round trip, integrity failures, review).
