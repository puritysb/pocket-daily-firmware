# Pocket monochrome content image v1

Status: firmware validator and companion codec/revision builder implemented;
**not connected to activation, rendering or a device capability yet**.

Based on Netpbm's [PBM minimal subset](https://netpbm.sourceforge.net/doc/pbm.html):
one raw P4 image, ASCII header, then packed raster. Pocket imposes stricter
canonical/bounded rules, not general-purpose PBM compatibility.

## Supported bytes

`P4\n<width> <height>\n` followed immediately by the raster. Width and height
are decimal integers 1–512, no leading zero, sign, comments, extra whitespace
or CRLF. Exactly one ASCII space separates dimensions; LF terminates each line.

Raster rows run top-to-bottom; pixels run left-to-right, MSB first within each
byte. One means black, zero means white. Each row uses `ceil(width / 8)` bytes.
Unused low bits in the last byte of every row must be zero. No trailing bytes,
second image or optional footer. Pixel bytes equal to LF, `#` or space remain
pixel data and must not be skipped as header syntax.

Maximum file size: 11-byte header + 32,768-byte raster = **32,779 bytes**.
PDCM's 64-KiB image-entry limit remains a coarse preflight bound; this semantic
validator enforces the tighter v1 image limit. Validation alone is not file
integrity: activation must also compare actual SHA-256 to the manifest.

Firmware `ContentImage.cpp` validates with a 64-byte row buffer, no allocation,
and publishes dimensions/row size/raster offset only on full success. Existing
`Bitmap` handles BMP, not this PBM payload; a bounded row-to-renderer integration
is still required. Do not label the decoder as actual on-device image display.

## Companion revision assembly

`ContentRevision(cards:images:)` now:

- Validates up to three cards, unique IDs, every image reference and canonical
  image filenames/content. Missing and unreferenced images are rejected.
- Serializes cards as `card-00-<id>.card`, `card-01-<id>.card`, etc. The numeric
  prefix preserves editor order under the canonical filename-sorted manifest.
- Calculates each file's exact size/SHA-256 and the PDCM manifest's full SHA-256
  revision, retaining immutable data for this local revision.
- Computes a local changed-file list relative to another constructed revision.
  A text edit need not resend an unchanged image. Reordering changes card paths;
  deletion can need only a new manifest, not asset uploads or old-file deletion.

This local comparison is **not a device receipt**. The transfer layer must
verify the identified device's actual stored files before skipping uploads.
No network request, SD write, automatic upload or firmware operation occurs in
the builder. Revision staging, file verification, commit/recovery, UI editor and
device confirmation remain separate outstanding P3 work.

## Shared golden

9×2 bitmap: `50 34 0a 39 20 32 0a aa 80 55 00`.
Both `test/content_manifest/ContentImageTest.cpp` and companion
`Tests/ContentRevisionTests.swift` pin these bytes. Tests cover dimensions,
padding, exact length, raster bytes resembling delimiters, read failures and
the 512×512 boundary. Companion tests additionally verify complete references,
duplicate IDs, immutability, small edits, ordering and empty revisions.
