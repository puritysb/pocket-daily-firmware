# Staging to immutable content publication

`ContentSealStore::sealRevision` is a storage operation called by the serialized
content activation endpoint. It accepts only a64-character lowercase
manifest SHA-256 revision and a supported-capabilities mask.

1. If `/pocket-daily/content/<revision>` exists, fully verify it. Return success
   only when valid; never overwrite even an invalid existing directory.
2. Otherwise verify `/pocket-daily/content-staging/<revision>` using the same
   manifest, file hashes, card/image semantics and references as published data.
3. Create the published parent if needed and rename the entire candidate
   directory through HalStorage. No candidate files are deleted or copied.
4. Fully verify the published location before returning success/counts.

This does not change either active record. The existing `activateRevision`
operation is a separate step, preserving the previous active revision until
that step succeeds. A completed move followed by failure or lost response is
ambiguous; retry checks the published data, without needing to re-upload it.
Every error clears output counts. Repeated successful calls are write-free.

## Ownership and limitations

- Caller must exclude all staging writes and serialize sealing/activation.
  Generic published-path protection does not lock mutable staging. A dedicated
  server session must close existing upload handles before invoking this code.
  The synchronous content endpoint now enforces writer admission; see
  `content-server-ownership.md`.
- Existing HAL rename and per-call storage locks are reused; they are not a
  multi-call transaction or evidence of FAT/controller power-loss durability.
  Boolean `exists` still cannot classify every storage error. No recovery code
  deletes an existing invalid destination; it reports the conflict.
- The manifest bounds its referenced payload, not total directory disk usage.
  Unreferenced staging files move with the directory but are never interpreted
  by the verifier. Best-effort retirement now reclaims an evicted revision's
  listed files without deleting unknown files; see `content-retention.md`.
  Global quota/admission and orphan/staging collection remain service work.
- Two bounded path arrays fit the checked stack budget. There is no new heap
  workspace; sequential verification calls use the existing transient workspace.
- No physical device, radio heap, power-cut or live-request validation occurred.

Host tests compile production sealing, verification and activation against the
fault-injecting HAL: normal publication, idempotency, invalid candidates,
existing-invalid destination preservation, failed and ambiguous rename,
post-move read failure, and preserving the old active revision until explicit
activation. The fake rename models whole-directory moves, not filesystem tears.
