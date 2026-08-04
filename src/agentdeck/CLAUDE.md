# src/agentdeck — Pocket sync substrate

**Fork-only code.** This directory never goes upstream. See the `fork-sync` skill before
re-porting the wire contract or preparing an upstream PR.

These files are a **TRIMMED hand-port** of AgentDeck's ESP32 wire contract, not first-party
AgentDeck code. The SSOT is `AgentDeck/docs/esp32-client-contract.md`. There is no codegen —
if you change a message shape here, you are diverging from the contract by hand.

## Product boundary

The X3/X4 product is an offline-first Pocket reader. AgentDeck is an invisible
content/sync provider, never the visible shell.

- Pocket Home contains the local continue-reading row and daemon-authored
  module cards only.
- New firmware requests `GET /feed?surface=pocket-reader`; session projections
  are excluded from that response.
- Live session state may remain parsed for backwards-compatible transport and
  diagnostics, but must not become a Home row, auto-surface a permission card,
  influence the device's power policy, or introduce AgentDeck branding/copy.
- Pocket choices, including neutral `choiceId: later`, persist to the Outbox
  before the card is removed.
- Wi-Fi absence is a normal state. Never replace saved reading content with a
  connect-to-AgentDeck screen.

The complete product contract is `docs/decision-card.md`.

## Attention steering invariant

An `awaiting_*` label alone **never** creates buttons.

- A real `requestId` creates the binary Allow/Deny `permission_decision` gate.
- Otherwise, only options whose `sessionId` / stamped `focusedSessionId` matches the selected
  managed session are actionable: `navigable` → `select_option`, non-navigable →
  `respond(shortcut)`.
- An observed session **without** a `requestId` is display-only. It must tell the user to
  respond in the terminal.
- **Never** synthesize options, and **never** map Deny to `escape`.

Getting this wrong sends a real command to a real agent session on the user's machine from a
button that should not have existed. The wire-level SSOT is
`AgentDeck/docs/esp32-client-contract.md` — check it rather than inferring intent from a label.

## Layout geometry

`eink_dashboard_layout.h` is **mirrored byte-for-byte** from AgentDeck's
`esp32/src/ui/eink/eink_dashboard_layout.h` by `scripts/sync-xteink-eink-dashboard.sh` (in the
AgentDeck repo). Do not hand-edit it here — edit it there and re-run the script with `--check`.
Everything else in this directory (GfxRenderer use, font loading, button hints, detail
interaction) is CrossPoint's own.
