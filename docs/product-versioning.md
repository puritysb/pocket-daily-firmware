# Pocket Daily firmware versions

Pocket Daily's first product version is **1.0.0**. Its CrossPoint source baseline is
**1.6.5** (`93e98bb78702e29868a16a13b80c40e6b36ccdff`); the pinned
FreeInk SDK and selected upstream ports are tracked independently in Git.
Product versions describe the Pocket Daily release and never claim to be a
CrossPoint release number. The firmware reports `firmwareLineage: 1` and
`crossPointBase: "1.6.5"` in `/api/status`; About shows the CrossPoint base.

Release tags use `pocket-v<version>` for stable and
`pocket-v<version>-beta.<n>` for a beta. The embedded version omits `pocket-v`.
For example, `pocket-v1.0.0-beta.1` embeds `1.0.0-beta.1`. Old `v1.6.6`
and `v1.7.0-beta.1` through `.4` tags remain historical test releases;
their tag names and assets must not be reused. The release workflow checks
that tag and embedded version agree. The beta channel takes only product tags;
the stable channel takes only stable product tags.
Local `firmware/LATEST_BUILD.txt` and versioned staging copies use the
embedded image version, including beta and development suffixes. Verify the
manifest and SHA-256 of `update.bin` before installing a local image.

Readers on the old public test images report no lineage. The companion app
recognizes the historical `1.6.6` and `1.7.0` series and offers the new product
release through **Update reader**. Firmware on those readers cannot be changed
retroactively: their built-in updater compares the old numeric series and
will not discover `1.0.0`. Use the updated companion or the documented manual
staging path to migrate. Once running lineage 1, normal numeric comparison
applies. Firmware built from this line ignores historical `v...` tags returned
by GitHub's latest endpoint while a new stable release is pending.

The `1.0.0-beta.1` prerelease is a test candidate, not hardware acceptance.
Before publishing stable `pocket-v1.0.0`, complete both device rows in
`docs/release-checklist.md` against the exact commit and image SHA-256, pass CI
on `main`, then tag. A host build or staging an image does not sign off X3/X4.
