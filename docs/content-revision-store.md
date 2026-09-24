# Read-only content revision verification

Implemented: `PocketDaily::Content::verifyRevision` and `verifyStagedRevision`
in `ContentRevisionStore.cpp`.
This joins the existing PDCM, PDCT and PBM codecs to
real firmware HAL file reads and mbedTLS SHA-256. It does **not** activate a
revision or add a network endpoint/capability.

## Candidate layout

`/pocket-daily/content/<revision>/manifest.pdcm`, with all referenced files
beside the manifest. `<revision>` must be exactly 64 lowercase hex characters,
equal to SHA-256 of the entire manifest. Manifest filenames remain bounded
leaf names; the candidate cannot choose a parent directory or absolute path.
The staged variant uses `/pocket-daily/content-staging/<revision>` with exactly
the same checks. Callers cannot supply arbitrary root paths.

The verifier:

1. Rejects invalid revision names and unavailable storage.
2. Checks manifest size and actual SHA-256 against the revision, then validates
   its schema, CRC, capabilities, file bounds and canonical filenames/order.
3. Opens every referenced file through HalStorage; checks exact length and
   SHA-256; applies PDCT/PBM semantic validation; rechecks the hash afterward.
4. Rejects duplicate decoded card IDs, missing image references and unreferenced
   images. One image may be shared by multiple cards.
5. Rechecks the manifest hash and publishes counts only on complete success.
   Every failure leaves `RevisionInfo` empty.

No files are created, deleted, renamed or modified. The error enum separates
manifest/file/hash/semantic/reference failures; hash failures can also represent
read errors and are not evidence that bytes on disk were definitively corrupt.

## Resource and ownership contract

- One temporary nothrow workspace, compile-time bounded below1,536 bytes,
  holds one decoded card, one entry, bounded path and at most three ID/image
  references. No whole revision or image is retained in RAM.
- SHA-256 uses64-byte file chunks; PBM uses at most64-byte rows. Hash helper
  asserts its local working objects fit the256-byte stack budget.
- HalStorage/HalFile retains the existing SD mutex behavior; no raw SdFat calls.
- **The caller must exclude directory writes for the entire verification and
  subsequent commit.** Rechecking hashes does not solve concurrent upload races
  or provide an exclusive filesystem lease. Do not expose activation until
  this ownership, active-state persistence and recovery are integrated.
- Verify does not authorize a sender, prove free space for staging, guarantee
  power-loss durability, perform garbage collection, or return an install receipt.
- There is no production caller yet. Radio-session heap/headroom, watchdog and
  physical SD behavior still need measurement once the service is integrated.

## Host evidence and portability

`test/content_store/ContentRevisionStoreTest.cpp` compiles the **production store
implementation**, with an in-memory fault-injecting HAL. Its SHA adapter uses
CommonCrypto on macOS or OpenSSL on other hosts (OpenSSL development headers/
library are required for those host tests); it does not use a dummy digest.
Production continues to use the existing ESP32 mbedTLS implementation.

The complete card+image golden matches companion `ContentRevisionTests`:
revision `77a7e4638640e81c9e380134ea28581928d55d001c0e625f492b7c106e5e2b8b`,
228-byte manifest,512-byte card and11-byte9×2 PBM. Tests assert no storage mutation
on success or failure and reject changed/missing files, invalid content with
correct hashes, duplicate IDs, bad references, unsupported capabilities,
unavailable/read-failing storage and unsafe revision names. Empty revisions pass.

These are host tests, not evidence of actual SD-card acceptance or installation.
An internal [two-slot active-state layer](content-active-store.md) now uses this
verifier for commit/recovery, and [sealing](content-seal-store.md) verifies staged
and published locations. Runtime staging/write exclusion and HAL error-classification
hardening are still required before exposure, followed by boot/local-card
rendering/actions and app/device confirmation.
