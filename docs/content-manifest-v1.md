# Pocket content manifest v1 (PDCM)

Capability extension: bit4 selects PDCT v2 alternate card layouts, as specified
in [content-card-layout-v2.md](content-card-layout-v2.md). Header/entry shapes are
unchanged; full verification checks this bit against actual decoded cards.
Readers lacking bit4 reject these revisions; old v1 revision bytes stay unchanged.

Status: **implemented in the local revision/activation runtime; physical acceptance remains pending.**
Companion `Sources/Studio/ContentManifest.swift` writes this format;
[`ContentManifest.cpp`](../src/pocket_daily/ContentManifest.cpp) checks its
structure with bounded reads. It is separate from `.pdl` and `.uipack`.
It enables a revision to describe small card and monochrome-image files without
rebuilding the firmware. ContentRevisionStore/ContentActiveStore provide the
separate validation/activation implementation; a manifest alone is not proof
of activation. Historical incremental notes below describe earlier stages.

## Canonical bytes

All integers are unsigned little-endian. No native struct serialization.

| Header offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 4 | ASCII `PDCM` |
| 4 | 2 | Schema version 1 |
| 6 | 2 | Header size 16 |
| 8 | 2 | File count, 0–16 |
| 10 | 2 | Required capability mask: cards=1, monochrome images=2, alternate card layout=4 |
| 12 | 4 | Exact manifest size: `20 + fileCount * 104` |

Each 104-byte entry follows the header:

| Entry offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 1 | Kind: card=1, monochrome image=2 |
| 1 | 3 | Reserved, all zero |
| 4 | 4 | Exact referenced-file byte length |
| 8 | 32 | Referenced-file SHA-256 |
| 40 | 64 | NUL-terminated ASCII relative filename, zero-filled tail |

The last four bytes contain CRC-32/ISO-HDLC of all preceding bytes: reflected
polynomial `0xEDB88320`, initial/final XOR `0xFFFFFFFF`. The revision identifier
is the full lowercase hexadecimal SHA-256 of the entire manifest, including
that CRC. There is no sender-selected revision field. Canonical entry order
makes reordered editor input yield the same revision.

## Limits and path safety

- At most three card files, each 1–16,384 bytes, with `.card` suffix.
- Image files are 1–65,536 bytes with `.pbm` suffix.
- Total referenced bytes at most 262,144; maximum manifest 1,684 bytes.
- A filename is 1–63 bytes, including suffix. Its stem begins with `[a-z0-9]`
  and continues with `[a-z0-9_-]`. No directories, extra dots, uppercase,
  Unicode, backslashes, embedded NUL or nonzero bytes after the terminator.
- Entries are strictly ASCII filename-sorted; duplicates are rejected.
- Capability mask is exactly1, plus2 iff any image exists, plus4 iff any card
  uses a v2 layout. The parser checks supported bits and image presence; full
  revision verification checks the layout bit against card bytes. Unknown or
  unsupported required capabilities are rejected before activation.
- Zero entries is a valid empty-content revision, not a failed download.

## Validation boundary and next integration

The firmware structural validator has no storage or network side effects, no
heap allocation, and reads at most one 104-byte entry at once. Its output is
empty on failure. Its source must remain immutable while validating.
It checks length/schema, limits, ordering, names, capabilities and CRC.

**It does not verify referenced-file SHA-256, authenticate the sender or install
anything.** Arbitrary 32-byte digests are structurally valid; the later file
verification pass must compare them to actual bytes. CRC is not authorization.
The golden `hello` payload is an opaque hash fixture, not a valid card example.

Historical P3 integration checklist (see current runtime status above):

1. The bounded [PDCT card codec](content-card-v1.md) and
   [PBM subset](content-image-v1.md) are implemented and tested;
   integrate both with on-device revision verification.
2. Stage an immutable revision directory and upload only missing/different
   files, using existing transport. Never interpret filenames outside it.
3. Validate every file's exact length, SHA-256 and semantic format; reject
   missing files/insufficient space without changing the active revision.
   The read-only [revision verifier](content-revision-store.md) now covers the
   file/hash/semantic/reference checks. Staging capacity and write exclusion
   still need integration.
4. Commit a checksum/generation-protected active-revision record, retaining the
   prior revision. Recover only a complete old or new revision on boot.
5. Report active revision/hash; reconcile lost activation responses by querying
   status instead of blindly replaying commands.
6. Connect the app editor, update progress and activation confirmation, then
   verify actual X3/X4 behavior. Do not reuse the firmware-install flow.

No route, status flag, UI editor, upload destination or disk layout for active
content is introduced by the codec alone. This is not a declaration that P3 is
complete or that the installed firmware accepts these manifests.

## Shared golden

Both repositories pin the same 124-byte manifest for `a.card`, size 5 and
SHA-256 of ASCII `hello`. Its manifest revision is
`f23bffd5b98b9c7a7ea75213b1c10d0c2691cb26ad372ba6f35f8c0dc2e534ae`.
Tests are firmware `test/content_manifest/ContentManifestTest.cpp` and companion
`Tests/ContentManifestTests.swift`. Mutation/truncation, unsafe paths, duplicate
ordering, unsupported capabilities and size/count boundary cases are covered.
