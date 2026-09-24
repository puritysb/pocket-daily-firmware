# Bounded retirement after content activation

The content activation endpoint now attempts to reclaim the single published
revision displaced from an active-record slot after a successful new activation.
It does not scan the card, delete staging, change the wire schema, end the session,
reboot, or initiate another transfer. Repeating the active revision is write-free
and produces no retirement candidate. Failed/ambiguous activation produces none.

## Protection and ownership

`activateRevision` returns an optional `RetiredRevision` only after the new record
has passed readback. The candidate must differ from both the new selection and
the preserved recovery slot. `retireRevision` independently opens and validates
both current records before deletion; missing, unreadable or corrupt records
refuse cleanup. It deliberately does not use boolean `exists()` to authorize
absence of a protected record. Both recorded revisions are protected even if a
later recovery would reject their payload.

The endpoint additionally pins the presentation host's revision in every
non-idle phase, including queued and failed views. If the presentation callback
is unavailable, cleanup is skipped. The main activity task owns activation and
presentation metadata; the render task only publishes phase/busy state. The
existing synchronous content-operation admission excludes uploads; watchdog
callbacks never dispatch another request or change presentation. Internal callers
must uphold that same ownership through the complete operation.

## Deletion scope and failure

The manifest must match its SHA-256 directory name and pass its schema, CRC,
filename and capability validation. At most16 listed assets are removed through
HalStorage, followed by the closed manifest and a **non-recursive** `rmdir`.
Unknown files/subdirectories, books, UI packs, staging and active records are
never deletion targets. The workspace is one checked temporary allocation of
at most272 bytes, reused across entries, not an unbounded directory inventory.
HAL file handles retain their existing checked allocations and storage mutex.

Failed asset removal leaves the manifest for a later guarded retry; already
removed assets do not need to verify again. Deletion can be partial. A failed
manifest removal also leaves the manifest. If the final directory removal fails
(for example because an unknown file remains), that file is preserved, but the
manifest may already be gone. Cleanup is best-effort after a confirmed activation:
its failure is logged and never changes the successful activation receipt.

## Scope not yet completed

Ordinary successful sequential activations with no displayed older revision
retain two complete revisions. This is not a global quota or full garbage
collector. A protected visible revision, interrupted/failed cleanup, previously
orphaned revisions, unknown files and abandoned staging may remain indefinitely.
There is no durable retirement queue, startup sweep, free-space admission or
automatic retry yet. Do not advertise bounded total SD usage or power-cut safety.
The separate [staging ingress bound](content-storage-admission.md) now limits
individual incoming files, not aggregate storage or actual free space.
FAT/controller-wide faults and physical radio/render concurrency remain unverified.

Host tests compile the production activation/retirement and storage code against
the fault-injecting HAL. They cover both record pins, displayed/unknown pins,
missing/corrupt/unreadable/false-missing metadata, corrupt manifests, partial
deletion/retry, unknown descendants, malformed paths and stale output after a
failed commit. Twenty sequential activations plus retirement retain exactly the
bounded two-revision set while recovery still works, including fallback from
corrupt newest content. No physical files or reader were deleted during testing.

Reproduce the local checks without a device:

```sh
cmake --build build/host-tests -j 4
ctest --test-dir build/host-tests --output-on-failure -R 'ContentStore\.(Retirement|RepeatedActivation)'
```
