# Pocket Daily firmware versions

The first public release is **1.0.0** (decided 2026-11-01; the 0.x series was
never published). There are two kinds of image only:

- **Release**: `platformio.ini`'s `version` exactly (`1.0.0`), built by the
  release workflow from tag `pocket-v1.0.0`, published as a normal GitHub
  release marked latest so `/releases/latest`, the reader's own updater and the
  companion all find it.
- **Developer build**: every local or CI build that is not a release,
  `1.0.0-dev-<branch>-<commit>[-w<worktree>]`.

Each published release gets a new version; one version is never reused for a
different image. Do not publish `1.0.0-beta.*`: the companion treats those
names as retired lineage-1 test releases and never offers them.

Its CrossPoint source baseline is **1.6.5**
(`93e98bb78702e29868a16a13b80c40e6b36ccdff`); the pinned FreeInk SDK and
selected upstream ports are tracked independently in Git. Product versions
describe the Pocket Daily release and never claim to be a CrossPoint release
number. `/api/status` reports `firmwareLineage` and `crossPointBase: "1.6.5"`;
About shows the CrossPoint base.

## Display

Screens never show the raw embedded string. Settings and About use
`FirmwareIdentity::displayVersion`, marked as Pocket Daily so a version never
reads as a stock CrossPoint one:

| Image | Embedded version (`/api/status`, updater, tag check) | Shown |
| --- | --- | --- |
| Release | `1.0.0` | `Pocket 1.0.0` |
| Developer build | `1.0.0-dev-<branch>-<commit>[-w<worktree>]` | `Pocket 1.0.0 dev <commit>` |

## Lineage

Version numbers alone do not order the series, so the firmware reports a
lineage (`src/pocket_daily/FirmwareIdentity.h`):

| `firmwareLineage` | Images | Notes |
| --- | --- | --- |
| absent | `1.6.6`, `1.7.0-beta.1`–`.4` | old public test images, tags `v…` |
| 1 | `1.0.0-beta.1`, early `1.0.0-dev-*` | built before the series was reset |
| 2 | `1.0.0` and up, current developer builds | current series; numeric comparison applies |

A reader on lineage 2 compares `pocket-v<version>` tags numerically
(`FirmwareVersion::isNewerStable`): `1.0.0` sees `pocket-v1.0.1` and
`pocket-v1.1.0` as updates, and reports "no update"
when a successful HTTPS check finds its own version at `/releases/latest`.
This comparison does not establish TLS availability: X3's current native
HTTPS path is memory-limited; see the supported update scope below. Tags without the
`pocket-v` prefix (the historical `v1.6.6`) are never offered.

Readers on lineage 1 or without a lineage migrate once through the companion
(**Update reader**) or the documented manual staging path; their built-in
updater cannot be changed retroactively. `1.0.0` reports lineage 2, which keeps
it distinguishable from the lineage 1 `1.0.0-beta.*` builds.

## Tags

- `pocket-v<version>`: a release, marked latest. The embedded version omits
  `pocket-v` (`pocket-v1.0.0` embeds `1.0.0`). This is the only published tag
  shape; candidates are tested as developer builds.
- The release workflow builds from `platformio.ini`'s `version`, refuses any
  other tag shape and checks that the image reports exactly the tag's version.
- Old `v1.6.6`, `v1.7.0-beta.1`–`.4` and `pocket-v1.0.0-beta.1` remain as
  historical test releases; their tag names and assets must not be reused.
  The companion ignores the retired `pocket-v1.0.0-beta.*` tags.

Local `firmware/LATEST_BUILD.txt` and versioned staging copies use the
embedded image version, including development suffixes
(`1.0.0-dev-<branch>-<sha>-w<fingerprint>`). Verify the manifest and SHA-256 of
`update.bin` before installing a local image.

As checked on 2026-10-25, no `pocket-v` release has been published and GitHub
latest is still historical `v1.6.6`. Current app release parsing excludes that
tag and retired `pocket-v1.0.0-beta.*`; no supported official image is available
through either current app channel. A local image or an earlier development
hardware result does not establish publication or production acceptance.

## Supported update scope for 1.0.0

Both X3 and X4 use the companion to download and validate firmware, stage it
on SD over Same Wi-Fi or Direct, and request installation through the reader's
own confirmation. Download before Direct connection; verify installation by
the same reader identity and reported version after reconnecting. SD recovery
with a previous compatible image remains required.

X3 reader-native HTTPS checking/downloading is unsupported in the current 1.0.0
scope because its stock TLS buffers exceed available memory. Its memory-failure
message and companion guidance must be verified, and recorded separately from
the successful companion/SD path. X4's certificate-verified native HTTPS check
and installation remain required. Neither model may silently install after
staging. Model-specific error, cancellation, validation and recovery criteria
are in [the release checklist](release-checklist.md).

## What a release still requires

A release is offered to every reader on lineage 2 and to every companion
on the stable channel, so it is not a private test build. Before tagging:
CI green on `main` for the tagged commit, and the exact release image
installed and exercised on an X3 and an X4 as far as
[the release checklist](release-checklist.md) lists for a development release,
including pre-tag model-specific update/recovery gates. Candidate identity, app
build, tester/date and physical results remain required even for 1.0.0. Checks
that require the new official release endpoint remain pending until publication:
the released image must match the tested hash, app release discovery must work,
and X4 must pass native OTA and the same-version no-update check before final
release sign-off.

`1.0.0` requires the full checklist with both device rows signed off against
the commit and image SHA-256. Before stable sign-off, resolve and test X3
native HTTPS or explicitly decide and publicly document its continued exclusion
from the product scope; the current 1.0.0 limitation does not automatically
approve that scope for `1.0.0`. A host build or staging an image does not sign
off a device.
