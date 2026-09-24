# Content candidate inspection endpoint

Implemented (not yet advertised as complete content deployment):
`POST /api/pocket/v1/content/prepare?deviceID=<8hex>&revision=<64lowerhex>`.
`deviceID` must exactly match `/api/status`. It is a routing check, not LAN
authentication. The endpoint is registered in the Pocket route table in each
server profile. No firmware installation or reboot occurs.

The caller first stages `manifest.pdcm` using the existing atomic file upload
under `/pocket-daily/content-staging/<revision>/`. Preparation may copy missing
files from the recovered active revision into that candidate. It never changes
published files, seals, activates or renders. Existing candidate files are not
overwritten by this optimization.

Successful JSON contains:

```json
{"schema":1,"deviceID":"1234ABCD","revision":"<64lowerhex>","fileCount":2,"verifiedMask":1}
```

`fileCount` is0..16. Bit0 of `verifiedMask` refers to the first manifest entry,
bit1 to the second, etc., in canonical sorted order. Bits outside fileCount
are zero. A set bit means the actual candidate file's size and SHA-256 matched
the verified manifest. A clear bit means missing, unreadable or mismatched;
the app must not skip its transfer. A previous published file counts only after
matching the candidate path/size/SHA, exclusive creation of its staging copy,
closing it and rechecking the actual copy's size/SHA. The complete manifest's SHA/schema/CRC/capabilities are
verified before inspection and its SHA is rechecked before sending the receipt.

This is byte verification, **not** semantic validity, reference completeness,
or activation success. Sealing still validates all card/image semantics and
references. A full mask must never bypass that final verification.

## Admission and runtime limits

- HTTP 409: live writer or wrong device identity. HTTP 400: malformed revision.
- HTTP 503: free heap below 8192 B or largest free block below 2048 B. These conservative
  guards are not measured hardware acceptance thresholds.
- HTTP 503 also reports failed reuse-file creation, copying or readback validation.
  No partial mask is returned: the caller must stop this deployment attempt, not
  interpret storage failure as a request to upload all missing files. The error
  asks the user to check SD health/free space; it does not prove a full card.
- HTTP 422: manifest cannot be verified, including storage/unavailable/OOM failures.
  No success receipt is emitted on such failure.
- Reads/copies run synchronously on the existing single server loop, with no upload
  dispatch while inspecting. The registered task watchdog is fed between64-byte
  hash reads. An unregistered task is not fed/logged repeatedly. This does not
  bound a stalled individual SD call or prove physical button responsiveness.
- Temporary inspection workspace is at most272B plus existing HAL/hash state;
  copying uses64B stack scratch and up to three concurrent HAL handles (manifest,
  source, destination). No whole-file or permanently retained allocation is added.
  response formatting uses192 stack bytes in a separate function. Nothing is
  permanently retained. Responses are `Cache-Control: no-store`.

The Swift `ContentPreparationReceipt` decoder checks schema, device, exact
revision, count and mask bounds before mapping set bits to immutable local asset
metadata. `CrossPointClient.inspectPreparedContent` calls this endpoint through
the existing per-reader HTTP queue and rejects non-success responses without
automatic retry. The companion ReaderContentTransport now stages the manifest
and assets; content/state and content/activate endpoints verify storage selection.
The editor exists; complete screen confirmation remains pending. This preparation
method alone does not stage the manifest. See `content-display.md` for the
reader integration and its limits.

Reuse is optional for absent/unrecoverable active content or a mismatched source;
these leave a clear receipt bit and the app uses its existing atomic upload.
Once a copy is attempted, creation/copy/readback failure aborts preparation with
`CopyFailed`, clears all receipt fields, and leaves prior published content intact.
Partial staging is not active content.
A later prepare never truncates such a partial file; upload replaces it. Reuse
checks only the same canonical filename in the active revision, not historical
revisions or renamed assets. Wire schema1 is unchanged and older read-only
prepare implementations remain compatible, with more data uploaded.

No free-space scan was added. The installed SdFat `FatPartition::freeClusterCount`
scans FAT entries and `MAINTAIN_FREE_CLUSTER_COUNT` defaults to0; running it
synchronously in every prepare request would add potentially long SD work with
no existing progress hook. A bounded space-admission/retention design is still
required; this change surfaces observed copy failure rather than predicting it.

Host tests use real hashes with a fake HAL and cover complete/partial candidates,
corruption, absence, read failures and invalid manifests. Swift tests cover all
two-file masks and malformed/mismatched responses. The endpoint binding and
watchdog helper are compile/source-verified, not exercised over live HTTP.
