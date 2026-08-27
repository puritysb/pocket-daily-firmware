# Pocket Daily product architecture

Pocket Daily Reader is the firmware product. CrossPoint is its reader/runtime
foundation, and AgentDeck is an optional Companion provider.

## Stable identity

| Field | Value | Purpose |
| --- | --- | --- |
| Product ID | `io.pocketdaily.reader` | OTA and Companion device identity |
| Surface protocol | `1` | Public compatibility contract version |
| Surface profile | `portable-reader/v1` | Requests a bounded portable-card feed |
| Update channel | `stable` | Third member of the OTA identity tuple |
| Product name | `Pocket Daily` | User-facing and Companion roster label |
| Current provider | `agentdeck` | Optional source attribution only |

Every HTTP Feed, Outbox, and OTA request sends the v1 Surface identity headers.
The legacy `surface=pocket-reader` query remains additive while the current
AgentDeck baseline migrates to header negotiation.

## Ownership boundary

Pocket Daily owns local books and progress, the daily-card model, cached glance,
the decision outbox, button grammar, sleep rendering, product strings, fonts,
and OTA policy. A Companion provider owns discovery, authentication, transport,
and conversion of its remote data into that bounded contract.

The Pocket Daily UI and models live under `src/activities/pocket_daily/` and
`src/pocket_daily/`. Some storage implementations remain physically under
`src/agentdeck/` for binary-schema compatibility, but expose Pocket Daily-owned
types and namespaces. Moving those remaining files is a mechanical follow-up,
not a protocol change.

## Compatibility and migration

The first successful read of `agentdeck-deck.bin` or `agentdeck-outbox.bin`
writes the corresponding `pocket-daily-*.bin` file, verifies the write result,
then removes the legacy file. Provider endpoint and auth caches keep the
AgentDeck name because they are provider-specific.

The firmware continues accepting the AgentDeck feed baseline. It requests the
legacy `surface=pocket-reader` projection while declaring
`portable-reader/v1` in the stable headers and client registration. Local
behavior remains available when discovery, Wi-Fi, or the provider is absent.

OTA fails closed: a Feed or WebSocket update is accepted only when
`productId=io.pocketdaily.reader`, the detected `board` (`xteink_x3` or
`xteink_x4`), and `updateChannel=stable` are all present and match exactly.
Older board-only staging is therefore ignored by this firmware; SD recovery
remains available while a Companion runtime upgrades to tuple-aware staging.

Provider discovery is service-based, not a fixed `localhost` or `.local`
hostname. Pocket browses `_agentdeck._tcp.local`, retains up to four IPv4 A
records from the selected SRV target, and tries them in order for Feed, Outbox,
glance refresh, and pull OTA. The `ip` TXT field is only a first hint; it is not
authoritative on a host running Ethernet and Wi-Fi simultaneously. The ADE2
endpoint cache preserves the full candidate set and promotes the last address
that completed a Feed response. Existing ADE1 single-address records migrate
automatically after one bounded mDNS refresh, without an unbounded
discovery-to-pull retry loop.

The repository-published [`agentdeck-surface.json`](../agentdeck-surface.json)
is the machine-readable integration declaration. It intentionally does not
advertise the optional WebSocket Inbox capability until that public runtime is
implemented and exercised.
