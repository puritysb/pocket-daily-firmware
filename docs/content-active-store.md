# Internal content activation records

Implemented in `ContentActiveStore.cpp`: `activateRevision` and
`recoverActiveRevision`. Pocket entry and the serialized content endpoints now
use these operations; physical acceptance and complete capability advertisement
remain pending. Generic network writers apply `ContentPathPolicy`. Successful
activation can return an evicted revision for guarded best-effort retirement;
see [content-retention.md](content-retention.md).

## Record and selection

Two76-byte files: `/.crosspoint/content-active.0` and `.1`.
Record v1 is explicit little-endian bytes:

- 0–3: magic `PDCA`.
- 4–7: generation1..UINT32_MAX.
- 8–71:64 lowercase hex characters identifying manifest SHA-256.
- 72–75: CRC-32/ISO-HDLC over preceding72 bytes.

A format change must use a new magic/layout contract. Exact size is required.
Newest valid generation wins; ties prefer slot0. Recovery verifies the entire
referenced revision, not just the metadata CRC. An unusable newest revision
falls back to the other fully valid revision. No valid record returns NoActive;
valid records but no verifiable content return VerifyFailed. Detected record
read errors with no recoverable alternative return ReadFailed.

## Commit procedure

1. Validate the candidate's actual files and references.
2. Inspect both metadata records; refuse detected read failures.
3. Find the currently recoverable revision. Preserve **that slot**, which may
   be older than the highest metadata generation if the newer content is bad.
4. If the candidate is already selected, return its existing generation without
   writes. Otherwise increment the maximum valid metadata generation, refusing
   wraparound, and write only the non-protected slot. If inspection classified
   that slot as missing, open with `O_CREAT | O_EXCL` (never `O_TRUNC`). A false
   negative from `exists()` therefore cannot truncate a slot that the subsequent
   filesystem open can see. Inspected valid/invalid non-protected records remain
   replaceable with truncation.
5. Close and read back exact record/CRC/revision/generation. Return success only
   after readback. There is no old-file deletion or rename.

If valid metadata exists but neither revision can currently be verified, refuse
the commit rather than guessing which record is safe to overwrite. Empty content
uses an ordinary valid empty-manifest revision; it does not require a tombstone.

Write/readback failure clears the result structure, but does not prove that no
record reached storage. `recoverActiveRevision` resolves the ambiguous outcome;
clients must not blindly retry or display an installation success. Candidate
file validation failures never reach metadata writing.

## Preconditions and limits

- Caller must serialize activation and exclude all writes to candidate/current
  revision directories through HTTP uploads, stream uploads, rename, deletion
  and ancestor-directory operations. Generic HTTP upload/commit, stream upload,
  WebSocket upload/abort, file-management and WebDAV mutation paths now reject
  the published content tree and active-record paths, including their ancestors.
- `ContentPathPolicy` compares ASCII case-insensitively and rejects ambiguous
  segments, control bytes, invalid FAT characters, trailing dots/spaces and `~`.
  The latter deliberately rejects even ordinary tilde-containing filenames to
  prevent short-name alias bypass. Read-only requests are unchanged. Prefix
  lookalikes such as `content-staging` are not the protected `content` tree.
- Future transfers must use `/pocket-daily/content-staging`, then a dedicated
  serialized seal/activate operation. Internal `ContentSealStore` now verifies,
  moves and re-verifies staging without changing active records; runtime
  ownership is enforced by the synchronous content endpoint (see
  `content-server-ownership.md`). Direct
  generic upload into `/pocket-daily/content` is intentionally forbidden. Internal
  writers must also obey ownership; a path check is not a transaction lock.
- This does not provide FAT/SD power-cut guarantees. Host fault injection models
  short writes, corruption and reported read failures, not controller caches,
  torn directory metadata or volume-wide failures.
- HAL `exists()` is boolean and cannot distinguish absence from every underlying
  I/O failure. The current Missing/Failed classification can only represent
  errors the HAL makes observable. Exclusive creation prevents overwriting a
  falsely missing target when open sees it; it does not make absence detection
  reliable or protect against volume-wide corruption. Audit that boundary before claiming
  universal preservation under arbitrary storage faults.
- HAL file wrappers now use the project's checked nothrow allocation before
  open/read/write/directory-next SDK operations. Empty handles have safe failure
  results rather than assertions; see `storage-allocation.md`. Content-store
  tests still use a fake HAL; separate allocation tests compile the production
  HAL against a fault-injecting allocation helper and SDK stubs. This does not
  prove every SDK/logging allocation can survive physical heap exhaustion.
- Runtime callers exist, but boot UI behavior, live radio heap and physical
  power-cycle behavior are not verified. No firmware upload was performed.
- Selection helpers use bounded local records; revision verification retains its
  existing temporary workspace and no full image/deck allocation is added here.

## Evidence

Production storage code is compiled into host tests with the fault-injecting HAL
and real host SHA-256. Tests cover activation/recovery/idempotency, every short
write boundary, every corrupted metadata byte, corrupt newest content, preservation
of the recovered older slot on the next commit, ambiguous readback, invalid
candidates and generation exhaustion. See `test/content_store/ContentRevisionStoreTest.cpp`.
Fault injection also hides existing slot0, newest slot1, and both slots from
`exists()` while keeping them visible to open. Activation fails without changing
any stored bytes; normal recovery succeeds once the injected fault is cleared.
Path-policy host tests cover protected ancestors/descendants/case variants,
ordinary transfers, staging, prefix lookalikes and ambiguous/invalid bytes.
Network handler integration has been source-audited and compiled, not exercised
as live HTTP/WebDAV requests or against real FAT aliases on a device.
