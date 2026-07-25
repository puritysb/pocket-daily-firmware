# Decision Card — the fork's product direction

This fork is evolving the X3/X4 from "an e-reader with an AgentDeck dashboard
tab" into a complete single-purpose device: a **decision interface**. The
CrossPoint core (HAL, e-ink driver, input mapping, WiFi/AP provisioning, SD,
fonts, i18n, OTA) is kept as the OS layer; the AgentDeck activity owns the
product grammar.

## The grammar

> **One screen. One question. At most four choices. One physical press.**

Everything the device shows is a *card*: a title/question/context block plus up
to four choice labels bound to the four front buttons. The device never grows an
app list — which card to show is always decided server-side (the AgentDeck
daemon), and the firmware only knows "the current card" and "what the four
buttons mean right now". Concepts like ONE (attention firewall), THREAD (work
checkpoint), PASS (human↔AI relay baton), NUDGE / PULSE / QUEST are all daemon
modules emitting cards — none of them are firmware features.

The ≤4-choice rule is enforced structurally: when a prompt carries more than
three options (slot 1 is always **Later**), the card degrades to the cursor
grammar rather than growing more buttons.

## Current state (M4)

- **Overview** (home): mission-control list of live sessions.
- **Card**: full-screen decision surface. Auto-surfaces from Overview when any
  session needs attention (never hijacks Detail, and waits for a
  2.5 s input-quiet window). Auto-resolves back to Overview when the prompt is
  answered anywhere.
- **Detail**: per-session timeline with the inline decision block as the
  fallback grammar (reachable from a card via the Detail softkey).

Softkey binding is **raw physical order** (left→right: `BTN_BACK`,
`BTN_CONFIRM`, `BTN_LEFT`, `BTN_RIGHT`) so the hint bar and the input can never
disagree via the user's logical remap:

| Attention mode | Slot 1 | Slot 2 | Slot 3 | Slot 4 |
| --- | --- | --- | --- | --- |
| PermissionGate (`requestId`) | Later | Detail | Deny | Allow |
| RealOptions (≤3 options) | Later | option 1 | option 2 | option 3 |
| RealOptions (>3 options) | cursor grammar: Later / Select / Up / Down |
| WaitingForOptions / RespondInTerminal | Later | Detail | — | — |

CJK option labels render in the body rows (SD CJK font); the hint bar falls
back to the row's keycap number because the theme hint font is Latin-only.
Dismissed prompts are remembered by content signature (sid + question +
requestId + option shape, 8-slot ring) — a card only re-surfaces when its
content actually changes. The honesty rules of the attention contract
(`src/agentdeck/attention_contract.h`, host-tested in
`test/agentdeck_attention/`) still hold: observed sessions without a
`requestId` never get synthetic buttons.

## Wire contract

Deliberately **zero new protocol**. The card is fed by what the daemon already
broadcasts (`sessions_list` per-session `question`/`promptType`/`options`/
`requestId`, plus the focused `state_update`), and answers with the existing
upstream commands (`permission_decision`, `select_option`, `respond`,
`focus_session`). A generalized `card_show` / `card_action` frame pair for
daemon-side card modules (THREAD, PULSE, …) is a future AgentDeck-side protocol
extension and must land there first (`shared/src/protocol.ts`), then be
re-ported here per the client-contract discipline.

## Roadmap

1. **M4 (done)** — Card view + direct softkey grammar, fed by session attention.
2. **M5 — boot-to-card**: a setting that makes the AgentDeck activity the home
   activity; the reader remains available as a library feature.
3. **M6 — power ladder**: today the card mode holds WiFi+WS (desk-class,
   `preventAutoSleep`). Pull-class cards (daily/slow content) need deep-sleep +
   periodic sync + the panel's zero-power static image. Push-class cards keep
   requiring live WS — do not promise phone-free real-time on battery.
4. **M7 (AgentDeck-side) — card protocol**: `card_show`/`card_action` with the
   ≤4-choice rule in the schema, shared with InkDeck (always-USB, same panel
   class — the natural first surface for push cards).
