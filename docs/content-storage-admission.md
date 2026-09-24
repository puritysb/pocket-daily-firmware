# Content staging ingress bounds

`ContentPathPolicy` now applies the existing `MAX_CONTENT_BYTES` (256 KiB) as a
per-file ingress ceiling anywhere under `/pocket-daily/content-staging`, including
temporary `.part` and `.davtmp` files. Matching is ASCII case-insensitive with a
segment boundary, consistent with FAT path protection; prefix lookalikes and
ordinary EPUB, firmware, learning and UI-pack destinations retain their existing
behavior. The separate generic path policy still rejects unsafe paths.

This is **not** free-space admission, a total staging quota, or manifest-authorized
file creation. Manifest validation still imposes its stronger file-kind limits,
file count, total referenced bytes and hashes. These checks now have an earlier
ingress backstop so one undeclared upload cannot grow without a content bound.

## Enforcement points

- Port82: the declared full length is checked before selecting/resetting staging,
  creating directories, opening or preallocating a file. Existing receiver length
  enforcement includes resumed prefixes.
- Legacy WebSocket: the declared length is checked before removing/opening a
  destination. Binary frames remain bounded by that declared length.
- HTTP multipart/chunked: the full joined destination determines the policy at
  START, even if a filename contains path components. WRITE checks the already
  accepted total (including the resumed prefix and buffered bytes) plus the next
  chunk before CRC/buffer/disk work. Overflow closes the file, drops the buffer
  and leaves a failed receipt; no successful commit follows. Partial data may
  remain, but never exceeds the ceiling because of a newly accepted chunk.
- WebDAV PUT: every chunk checks the actual temporary file length. A refused
  chunk closes/removes that request's temporary file and never replaces the
  original destination. MOVE/COPY into staging require a readable non-directory
  source within the bound before either handler can remove a destination.
- Browser rename/move and Pocket atomic commit check destination size before
  replacement. Directory moves into staging through WebDAV are refused because
  they would bypass per-file upload checks.

The range predicate uses subtraction after checking the existing offset, never
an overflowing `offset + count`. It is allocation-free. HTTP retains one Boolean
classification in its existing upload state instead of building a path per
chunk; other paths reuse their existing strings/handles. WebDAV MOVE/COPY adds
one scoped existing HAL handle for admission, released before the handler runs.
This inherits the HAL's checked allocation and storage lock.

## Free-space work remains

The SDK `SDCardManager` keeps its SdFat volume private and still exposes no
free-space query (the mounted format patch below does not add one).
SdFat's `FatPartition::freeClusterCount()` may synchronously scan
the entire FAT when its cache is not available. Adding that call directly to a
radio request would introduce an unbounded storage traversal without an internal
watchdog/progress callback. Do not put a whole-volume scan in transfer admission.
The installed configuration has MAINTAIN_FREE_CLUSTER_COUNT=0; turning it on
would still require an initial full count, and does not establish a reservation.
Existing preAllocate remains an optimization, not a required admission check:
failure can mean lack of a contiguous span on an otherwise usable fragmented
card. Allocation/write errors must preserve the prior published/active content.
A package passing ingress bounds can still encounter a full card.

Aggregate staging quotas, file-count enforcement before upload, abandoned
transfer cleanup and durable retirement retry remain unfinished. Many bounded
files can still fill a card. This is not authentication or denial-of-service
protection against an untrusted LAN client.

### Directory accounting prerequisite (source audit 2026-09-22)

Do not implement quota admission by summing `HalFile::openNextFile()` until it
returns an empty handle. `lib/hal/HalStorage.cpp` currently returns that result
both when allocation fails before advancing the directory cursor and when the
SDK does not open another entry. The existing HAL allocation test proves the
former is possible without reaching directory end.

Checking only the parent's SDK `getError()` would still not prove a complete
inventory. In the installed SdFat `FatLib/FatFile.cpp::openNext`, an LFN checksum
mismatch and `openCachedEntry` failure also jump to `fail`; they are not required
to set the parent directory's read-error flag. `FsLib/FsFile.cpp::openNext` then
discards its child implementation pointer after failure. A clean-looking parent
and an empty child therefore do not distinguish all corrupt-entry failures from
normal end. `openNext` can also scan arbitrarily many deleted/LFN entries inside
one call; a bound on returned files alone is not a bound on SD work.

The next storage primitive must distinguish confirmed end, entry, I/O/corruption,
allocation failure and work-budget exhaustion, with a bound on raw entries or
sectors processed per step. Unknown/incomplete inventories must refuse quota
admission, not be counted as empty. Implement this at the HAL/SDK boundary with
deterministic EOF, corrupt-LFN, read-failure and long-deleted-directory tests;
do not bypass the storage mutex or label the current wrapper an exact iterator.
Capacity snapshots also need invalidation on writes/remount and exclusive
reservation ownership; an old free-space number is not a reservation.

### Retired whole-volume scanner (2026-09-22)

The unused AllocationScan, metadata/allocation HAL extensions and dedicated
SdFat/SDK patches were removed after evaluating their place in the transfer
path. One-cluster steps bound each call but still require work proportional to
the entire card; publishing only complete totals does not make that scalable.
There was no production caller, reservation or measured latency benefit.
Neither a whole-card walk nor mandatory contiguous preallocation becomes a
new prerequisite for an edit/apply cycle.

The production format query and DirectoryAccounting stay: they validate a
bounded revision directory at seal, not every cluster on the card. Existing
verified publication/activation and duplicate staging reclamation also stay.
Aggregate staging limits, orphan handling and full-card failure acceptance
remain unfinished; removing the scanner does not declare those solved.
The removed local source is recoverable in build/retired-allocation-layer.tar.
Do not restore it as transfer preflight merely because prior logs tested it.

HAL foundation follow-up: `HalFile::readDirectoryRecord` now reads one raw
32-byte FAT/exFAT slot under the storage mutex. It returns Record, physical End
(zero marker or clean EOF), or Error; partial reads, sticky SDK error flags,
non-directory/empty handles and unaligned cursors fail. The output is cleared on
End/Error. It includes deleted and LFN slots and performs no additional handle
allocation, so callers can count raw work instead of returned names. Existing
`openNextFile` behavior is unchanged. Callers use a dedicated cursor, stop at
End/Error and enforce their own total work budget/yield schedule.

This is a bounded number of SDK read calls, not a real-time bound on physical SD
latency. It deliberately does not decode file names, validate LFN/entry-set
checksums, identify filesystem type or total an inventory. A decoder must reject
an incomplete entry sequence even when the raw cursor returns physical End.
This directory primitive alone supplies no quota, free-space receipt or storage
reservation. Incomplete inventory must never look like available capacity.

HAL host tests cover deleted-slot work accounting, allocation-free iteration,
clean EOF/zero markers, partial/negative reads, pre-existing and newly raised
SDK read errors, invalid handles/types/alignment and cleared failure output.
They execute the production HAL against a fake SDK, not a real FAT image or SD
card. Filesystem decoder fixtures and hardware latency remain separate gates.

### Mounted format identification

`Storage.filesystemFormat()` reads the mounted SDK type under the existing
storage mutex: FAT12/16/32 map to Fat,64 to ExFat, and unmounted/unknown values to
Unavailable. It performs no directory/FAT scan, allocation or write. Callers must
not guess the format from a filename or slot contents. A scan must hold exclusive
lifecycle ownership so its directory handle and format refer to the same mount;
the query is not a remount-safe snapshot or capacity reservation.

The SDK accessor is a reviewed two-line patch in `scripts/storage_sdk.patch`,
applied by the base PlatformIO pre-script `scripts/patch_storage_sdk.py`. The
submodule pin is unchanged; its working header receives the patch at build time.
Reverse-check detects an already-applied patch; missing/incompatible input stops
the build before mutation. Unrelated source edits are preserved. Keep the patch
and pre-script with the HAL consumer rather than relying on a local-only SDK
edit. Python tests exercise apply/idempotence, incompatible-source refusal and
missing-patch refusal. This does not expose the raw SdFat volume outside the HAL.

### Bounded logical accounting

`DirectoryAccounting` consumes those raw slots with a caller-supplied mounted
format. Each `step` performs at most one HAL record read; file/directory count,
logical file bytes and total slot-read work all have caller-selected limits.
Deleted/LFN slots count toward the work limit. A caller must retain exclusive
scan ownership, yield between steps and provide finite product limits. The
state is at most128B with no allocations. No recursive descent occurs: child
directories are counted, their descendants and allocated cluster bytes are not.

FAT parsing checks LFN ordinal/checksum linkage, rejects interrupted sequences,
counts short entries and excludes valid dot entries/volume labels. exFAT parsing
checks the file/stream/name sequence, secondary count, valid/data lengths and
entry-set checksum. Unknown entry types/extensions are Unsupported rather than
silently skipped; known root metadata is not counted as a user file. This is
structural accounting, not complete Unicode/name legality, upcase/name-hash,
cluster-chain, allocation bitmap, filesystem repair or corruption certification.

Only Complete publishes totals. Invalid/Unsupported/I/O failure/limit exhaustion
are sticky terminal states and expose no partial totals. Logical byte limits use
subtraction before addition to reject64-bit overflow. A physical end during an
unfinished LFN or exFAT entry set is Invalid, not a successful empty inventory.
Pure record fixtures plus production-HAL/fake-SDK tests cover these cases; they
are not real-volume/SD-card acceptance tests.

The accountant is not used to admit individual uploads. Integration still needs
aggregate traversal of staging revision directories and reservation/free-space
admission. Counting logical sizes alone must never be presented as available
card capacity.

### Seal-time exact inventory gate

`ContentSealStore` now uses the accountant under its existing exclusive
seal/activate ownership. After manifest/assets pass full hash/semantic checks,
the candidate directory must contain exactly `fileCount + 1` regular files,
zero child directories, and exactly `contentBytes + 20 + 104 * fileCount` logical
bytes (including the manifest). At most512 raw record reads are permitted,
including deleted slots and final end confirmation. Each step calls the existing
watchdog-only progress hook; it must never run another writer. The local scanner
and one HAL directory handle are released before rename. No persistent inventory
or additional dynamic payload buffer is allocated.

The same gate checks an already-published revision on idempotent seal and checks
the target again after rename. Before-move failure preserves staging; post-move
failure remains ambiguous/failed and does not activate. Existing result types and
HTTP422 behavior remain unchanged. Extra files (even empty), subdirectories,
unknown formats/extensions, I/O/corruption and exhausted raw-work budgets refuse
seal. Leftover `.part`/backup files are not silently deleted to make it pass.
The user/app must resolve such leftovers separately; no automatic retry/cleanup
protocol is added. Existing active records are untouched by sealing.

This gate prevents unlisted objects travelling with a sealed directory, not
staging from occupying the card before seal. It is neither aggregate quota nor
free-space reservation. Store integration tests use a fake tree synthesizing FAT
slots; separate decoder fixtures cover exFAT. Neither proves physical media,
power-loss behavior or a complete server integration.

Host tests exercise case/segment matching, ordinary-file compatibility, exact
limits, resumed prefixes, cumulative chunks and UINT64 overflow. Production
handlers are source-audited and firmware-built; these tests do not simulate the
full HTTP/WebDAV server or prove physical SD/radio behavior. Swift's existing
manifest/asset limits remain stricter, with no wire schema or app change needed.

### Reapplying a published revision: duplicate staging reclamation

The idempotent seal branch previously returned immediately after verifying an
existing published revision. If the app had staged that same revision again,
the duplicate remained under content-staging even after successful activation.
This was a concrete accumulation path, independent of radio reliability.

That branch now checks for a staging copy and verifies its manifest hash,
remaining asset hashes and exact flat inventory before cleanup. The immutable
published copy has already passed full hash/semantic/inventory verification,
so matching asset hashes bind the remaining subset to those validated bytes.
Only manifest-listed staging
assets, then its manifest, then the empty directory are removed. No recursive
deletion or global staging scan occurs, and active/recovery records are untouched.
Unknown/extra/nested files, corrupt/unreadable assets and incomplete inventory
prevent cleanup without invalidating the verified published revision.

The caller retains exclusive seal/activate ownership; progress callbacks only
feed the watchdog. Cleanup reuses the existing <=272B fallible temporary
manifest-deletion workspace, released on return, with no persistent buffer or
directory list. Verification allocations finish before deletion starts. HAL
locking and the <=512-slot exact-inventory limit remain unchanged.

A deletion failure can leave a partial staging copy; it is logged, but the
published seal result remains successful so the app does not resend an already
valid revision. A later explicit seal can resume cleanup when the retained
manifest is valid and every remaining file is verified. The inspector totals
only verified assets; any existing unverified/undeclared file makes the exact
inventory count or size disagree and prevents deletion. Already-removed assets
need not be recreated. A manifest-only remainder can also be reclaimed.
No timer or background retry is added. Missing/corrupt manifests remain
unreclaimed, including an empty directory left after manifest deletion and a
failed rmdir. General abandoned-staging collection,
aggregate quota and free-space reservation are still unfinished. This is not a
claim that a full card can always accept an update.

Production-store host tests reproduce the former accumulation, repeat cleanup
without changing active records, preserve corrupted/incomplete/unexpected trees,
refuse cleanup when published content is invalid, and inject a mid-delete
failure and then resumes it. Subset tests include one/no assets remaining and
an extra zero-byte file that must prevent deletion. They use the fake SD tree,
not a physical SD card. SealResult, HTTP
responses and the Swift activation confirmation contract are unchanged.
