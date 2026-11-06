# Pocket Daily installer plugin for stock CrossPoint

Layer C of the CrossPoint 2.0 distribution plan
(`docs/upstream-integration-2.0.0.md`). A CrossPoint 2.0 SD-card plugin that
lets a stock X3/X4 user download Pocket Daily firmware from the reader itself:

1. `device.json` is a JSON catalog over `releases.json` on this repository's
   `main` branch (`raw.githubusercontent.com`).
2. Fetch saves the selected release's `firmware.bin` as `/pocket-daily.bin`,
   the same name the companion app stages (`PocketModel.crossPointStagedFirmwareName`).
3. The user installs it from Settings → System → SD Card Firmware Update; stock
   validates the image (segments, checksum, SHA-256 trailer, board tag `x4`)
   and asks for confirmation. The plugin never flashes anything.

Stock 2.0 behaviour this relies on, read from CrossPoint `42d58daa`:
`download.url` must be set to `{url}` for a JSON catalog; an empty
`dest_dir` saves to the SD root; downloads stage in `<dest>.part` and replace
an existing file; redirects (GitHub release assets) are followed.

## Releasing

- `releases.json` lists only published, hardware-signed `pocket-v<version>`
  releases. Add an entry in the same change that records the release; the URL
  must be the release's `firmware.bin` asset. `scripts/test_crosspoint_plugin.py`
  enforces the shape.
- The payload `version` changes only when `manifest.json`, `device.json` or
  `README.md` change; release entries do not need a plugin release.

## Publishing (owner approval required)

Plugin Hub discovers a public repository that carries the `crosspoint-plugin`
topic, exactly one direct-child `<name>.crosspoint-plugin/` folder and a
non-prerelease GitHub Release tagged with the manifest version. This folder is
not a direct child and this repository has no such topic, so nothing here is
published. Publishing means creating that repository (or a release-asset
entry in Plugin Hub's `whitelist.json`) and is an outward action: the owner
approves the exact repository, payload and release text first.

Manual install for testing: copy `pocket-daily-installer.crosspoint-plugin/`
to `/plugins/pocket-daily-installer/` on a stock CrossPoint 2.0 card and enable
Settings → Plugins.
