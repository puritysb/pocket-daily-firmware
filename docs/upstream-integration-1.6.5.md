# CrossPoint 1.6.5 baseline integration

Prepared on 2026-09-29 on `feature/upstream-1.6.5`. The user authorized commit,
main integration and deployment. Publish the next beta after green PR/main CI;
physical X3/X4 acceptance remains pending and gates the stable release.

## Pinned inputs

- Product parent: `ccc601c5de1a8c5acad5076b6c78ec79687fa702`.
- Previous common upstream baseline: CrossPoint 1.4.1,
  `2754a5ff01644d36cf0a17db98f28408666ba518`.
- Integrated upstream tag 1.6.5: `93e98bb78702e29868a16a13b80c40e6b36ccdff`.
- FreeInk SDK: `111fdcc7f0176c3ee38391a160ee296bf492dbd8` plus the reproducible
  `scripts/storage_sdk.patch`. The SDK working tree is intentionally dirty only
  for that build-managed patch; the gitlink pins the unmodified upstream commit.
- Companion contract verification: app main
  `2757e8051593696a07330c713040f44f9c128269`.

The product version remains **1.7.0**. CrossPoint's baseline number and the
Pocket Daily release number are separate: lowering our product version to
1.6.5 would misidentify an upgrade. A future release tag still requires the
release checklist and explicit publication authorization.

## Integration decisions

| Boundary | Resolution |
| --- | --- |
| SDK / hardware | Replace open-x4-sdk with pinned FreeInk SDK, retain the HAL and X3/X4-only product profiles, single framebuffer, tuned C3 core and bounded NimBLE configuration. |
| EPUB layout | Adopt packed word storage, visible-text LUT, links, spacing and RTL changes; retain bounded incremental pagination, low-heap suspension, layout ahead, bilingual mode and exact raw text positions. |
| Cache | Reserve section version **134** for the combined layout, including raw page offset prefix and five LUTs. Partial caches include the version tag. Version 133 and changed spacing must rebuild. |
| Reading sync | Raw DOM character offsets remain the companion v1 contract. Upstream visible offsets are separate layout coordinates. Both resolver paths have regression coverage against the same 430-position golden data. |
| Saved progress | Keep the seven-byte product prefix. Read historical 4/6/7-byte and upstream 10-byte records; write seven bytes or a tagged 12-byte extension containing the visible offset. Reject malformed lengths/tags. |
| Fonts / rendering | Adopt bounded glyph scans and upstream typography while retaining C3 bounded UI fonts, fallback coverage, metric release and on-demand glyph loading. Page prewarm replaces resident glyphs rather than retaining an expanding union. |
| App preferences | Adapt app font-size slots 0..3 to upstream point sizes. Home keeps a single Pocket Reader entry, with Articles beneath it. |
| HTTP / OTA | Retain certificate-verified ESP-IDF transport and streamed OTA with product `firmware.bin` naming and board validation. Keep the existing two OTA slots and partition table. |
| KOSync | Retain verified ESP-IDF TLS, add upstream account creation, 2xx/204 handling, metadata and rich position fields. Responses have a 4 KiB cap, a failure latch and one nothrow allocation before TLS. |
| Maintenance | Pin sync inputs, dry-run before merge, reject dirty/wrong-branch merges, never change branches or commit automatically. Add compile-time dependency boundary checks and build both development and production profiles in CI. |

A naive combined build linked both ESP-IDF TLS and wolfSSL and exceeded the
existing 6,553,600-byte OTA slot. The C3 product uses one verified TLS engine;
`SecureNet`/wolfSSL are not enabled. This does not adopt upstream's insecure
TLS mode or promise TLS-1.3-only server support. A host request limited to TLS
1.2 received HTTP 200 from `sync.crosspointreader.com`; that is not a device
handshake or sync acceptance test.

The HTTP, raw-coordinate and bounded-font differences above are deliberate
fork adaptations, not claims that every upstream implementation was copied
unchanged. Future integrations must review these boundaries explicitly.

## Verification

- Full host suite: **828/828 passed**.
- Sync safety: 8 tests; dependency boundaries: 6 tests plus live tree check;
  SDK patch application: 3 tests; Apple artifact provenance: 6 tests;
  Home/network routing: 13 tests; native cppcheck adapter: 1 test. All passed.
- Real PocketSansWorld font fixture: 48 production-rasterized frames compare
  Cached vs BoundedUI for regular/bold Latin, Korean and Japanese across X3/X4
  geometry and four orientations; guards intact. C ABI matches 40 page frames,
  including refusal/recovery behavior. This is host rendering only.
- Companion iOS: ReadingSyncTests, ReaderEngineTests and NearbySyncProtocolTests,
  **101/101 passed**. macOS XPointer-to-text cross-check: **1/1 passed**.
- Apple renderer: macOS arm64/x86_64, iOS arm64, simulator arm64/x86_64 archive
  built and provenance verified. SDK inputs are now included in source hashes.
  The companion's checked-in XCFramework is not replaced by this firmware task.
- Strict cppcheck 2.11: no defects. The standard package mirror failed during
  the final run, so the official 2.11 source was built locally and used through
  `scripts/check_firmware.py --cppcheck build/cppcheck-2.11/cppcheck` with the
  same PlatformIO flags and fail-on-defect levels; the actual child process
  path was verified. CI builds its pinned native 2.11 baseline too.
- Development and production builds succeeded with no compiler warnings or
  errors. Both retain the original 6,553,600-byte OTA slots; image identities
  are recorded below. Static RAM/flash reports do not measure runtime heap.

Local logs are under `/tmp/pocket-165-*`; they are not release evidence in CI.
Build artifacts and intermediate caches remain ignored. Tests that use host
HALs do not demonstrate physical SD, radio, watchdog, OTA or heap behavior.

## Remaining acceptance and rollback

Complete the new major-upgrade section of `docs/release-checklist.md` on both
X3 and X4. Use File Transfer -> Join a Network plus `scripts/pocket_put.py` to
stage the selected image; the user performs installation and signs off the
physical device results. Keep the previous product image and an SD backup.

The integration commit retains the pinned upstream parent. Publication follows
green PR CI, main merge and green main CI. Beta tags are hardware-test candidates
under the release checklist; a stable tag requires both hardware sign-offs.

Cache v134 intentionally rebuilds old section caches. Downgrading should also
rebuild incompatible section caches; the new tagged progress extension and
upgraded settings need an explicit backward-compatibility check during the
rollback hardware exercise. Keep the previous SD data backup rather than
assuming a firmware-only downgrade restores every persisted preference.

## Final local artifact identities

These images come from the uncommitted candidate. Their parent SHA is not a
release commit; use the image SHA-256 to identify them. The staging directory's
`update.bin` currently contains the development image. No image was uploaded
or installed. Both build logs contain zero compiler warnings and errors.

| Environment | Bytes | Remaining OTA slot bytes | SHA-256 |
| --- | ---: | ---: | --- |
| default | 6,407,472 | 146,128 | `b820ef86d29197013834906dae60ffff0b0c63d25d11ccbc00c051ce1b52a1d7` |
| gh_release | 6,351,200 | 202,400 | `a83e2ea69f6614bfafca7327af65c284621efff3e2c1f5ac2f3c477bb685a49d` |

The development image leaves about 143 KiB (2.2%) of the existing slot. This
is a real limit for future features; retain both build-size gates in CI. The
companion's `FirmwareRelease.maximumBytes` is the same 6,553,600-byte limit.

The formatter is idempotent. Upstream/vendor trailing whitespace remains in
imported prose/license/miniz files and the exact-match pioarduino patch string;
those are outside the C/C++ formatting gate. The only unstaged SDK change is
the eight-line build-managed SDCardManager API patch.

Local evidence: `firmware/UPSTREAM_1.6.5_VALIDATION.json` records image hashes
and the checked source fingerprint; `build/integration/verified-source-files.json`
lists the individual source hashes. They are ignored outputs, not release files.

## CI reconciliation

The first remote run found a mid-build tool upgrade: the custom Arduino rebuild
upgraded PlatformIO 6.1.19 to 6.2.0 and replaced SCons 4.8 while it was still
running. CI/release jobs now start on pinned 6.2.0 with a separate cache key.
Linux cppcheck also found an identical conditional result on non-USB boards
and an unused/shadowed theme coordinate; both were simplified without changing
reader behavior. Remote CI remains the publication gate after these fixes.

A fresh CI run also exposed the platform's SCons 4.8 package overriding Core
6.2's SCons 4.11.1. `base.platform_packages` now pins the matching SCons archive;
the native cppcheck adapter retains that inherited pin. The obsolete workaround
that required an already-cached global cppcheck package was removed.
