# Pocket Daily Firmware Project Memory

This is concise, repository-owned context for future sessions. It is not a chat
transcript. Current source and release records override dated observations.

## Repository split

- Firmware: `/Users/puritysb/github/pocket-daily-firmware`
  (`https://github.com/puritysb/pocket-daily-firmware`)
- App Store app: `/Users/puritysb/github/pocket-daily`
  (`https://github.com/puritysb/pocket-daily`)

The firmware repository owns device behavior, endpoints, local persistence,
memory gates, and flashing. The app repository owns the Apple-platform client.
Nearby Sync v1 is a contract between them; do not change only one side.

The old combined repository and its generated memories used the path
`crosspoint-agentdeck` and mixed firmware, AgentDeck, and app concerns. Do not
copy those memories wholesale. Promote only a verified, firmware-relevant fact.

## Git topology

- `origin`: `puritysb/pocket-daily-firmware`
- Product branch: `main`
- `upstream`: `crosspoint-reader/crosspoint-reader`
- Upstream foundation branch: `upstream/master`

Pocket Daily product directories are intentionally downstream-only. Use the
repository sync script and merge upstream; never rebase the product history.

## Durable release constraints

- X3 and X4 are no-PSRAM ESP32-C3 devices.
- `docs/nearby-sync-v1.md` defines mandatory runtime heap and responsiveness
  gates for both models.
- A host build and host unit tests are necessary but do not satisfy hardware
  sign-off.
- `firmware/LATEST_BUILD.txt` identifies the locally staged ignored artifact.
- GitHub releases are the production OTA source; `firmware.bin` must be present
  as an exact release asset name.
- App Store timing is independent, but Nearby Sync v1 must be frozen and
  compatible before either side is presented as production-ready.

## Maintenance

Record only durable decisions, verified baselines, protocol contracts, and
release evidence. Date mutable facts, name their source of truth, and replace
stale notes rather than accumulating contradictions.
