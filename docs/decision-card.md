# Decision Card — the fork's product direction

This fork is evolving the X3/X4 from "an e-reader with an AgentDeck dashboard
tab" into a complete single-purpose device: a **decision interface**. The
CrossPoint core (HAL, e-ink driver, input mapping, WiFi/AP provisioning, SD,
fonts, i18n, OTA) is kept as the OS layer; the AgentDeck activity owns the
product grammar. One firmware serves both devices (runtime X3/X4 detection).

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

## The shell: Face / Deck / Outbox

E-ink retains its image at zero power, so the device's natural mode of being is
**"a surface that always shows something"**, not "an app you open". Three
layers:

- **Face** — the home surface. Boot lands here and it renders *immediately*
  from whatever is known; joining Wi-Fi, discovering the daemon, and connecting
  are a **status line inside the Face**, never a screen that replaces content.
  The Wi-Fi picker appears only on first run (no saved credentials) or when the
  background join times out.
- **Deck** — the bounded set of active cards. Fed live from daemon state and
  persisted to SD (M5.5, `src/agentdeck/deck_store.*`): the Face renders the
  last-synced deck at boot and whenever the daemon is unreachable, with an
  honest "as of" sync age. Cached cards are display-only — they never gain
  buttons (acting offline is the M6 Outbox).
- **Outbox** — (M6) decisions are recorded locally first and pushed when a
  connection exists, so being offline never blocks pressing a button.

### Card validity classes (`actionClass`, M6 contract)

Offline behaviour is decided per card, extending the attention contract's
honesty rule:

| Class | Examples | Offline behaviour |
| --- | --- | --- |
| `live` | PASS approval, PermissionGate | greys out + "reconnect to act"; TTL-expires. Never lets the user press an approval that cannot be delivered |
| `day` | NUDGE, QUEST, INTERVAL, FORK | valid all day; answers queue in the Outbox |
| `info` | THREAD checkpoint, PULSE digest | read-only; always valid; shows sync age |

"Personalised assistant while offline" therefore means: the daemon (the brain)
precomputes the day's cards and the device carries them as a cache of prepared
decisions. No on-device LLM is implied, ever.

## Current state

- **Face** (home): mission-control list of live sessions, rendered in every
  connection state with a status line (`Live · ip · n` / `Joining Wi-Fi …` /
  `Searching for AgentDeck…` / `Connecting …`). Background STA join with saved
  credentials — no blocking picker after first run.
- **Deck persistence (M5.5)**: while connected, the deck (alive sessions'
  display fields) is written to `/.crosspoint/agentdeck-deck.bin` whenever its
  content signature changes (throttled, tmp+rename). At boot — and after a
  daemon loss — the Face renders the cached deck with a bold
  `Last synced deck · as of Xh ago` line (age appears once a clock source
  exists: NTP after Wi-Fi, or the daemon-clock estimate at save time). Cached
  rows suppress the attention banner, keyboard focus, and the Open hint; live
  data always wins the moment it arrives (`dataReceived` chokepoint).
- **Card**: full-screen decision surface. Auto-surfaces from the Face when any
  session needs attention (never hijacks Detail; waits for a 2.5 s input-quiet
  window). Auto-resolves back to the Face when the prompt is answered anywhere.
- **Detail**: per-session timeline with the inline decision block as the
  fallback grammar (reachable from a card via the Detail softkey).
- **Boot-to-card**: Settings → System → "Start on power-on" = Home / Agent
  Dashboard (`startupApp`). Holding **Back during boot** is the escape hatch to
  the reader home; exiting the dashboard also lands on Home.

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

The docked live mode remains **zero new protocol**: the card is fed by what the
daemon already broadcasts (`sessions_list` per-session `question`/`promptType`/
`options`/`requestId`, plus the focused `state_update`), and answers with the
existing upstream commands (`permission_decision`, `select_option`, `respond`,
`focus_session`).

**M6 pull sync landed in AgentDeck first** (client-contract discipline —
AgentDeck commit 89f538c5, `shared/src/protocol.ts` § Card Feed Pull Sync,
served by the Node daemon):

- `GET /feed` → `card_feed` — one card per session (`cardId:
  "session:<sid>"`, body = the same `SessionInfo` shape as `sessions_list`),
  each stamped with `actionClass` + `expiresAt`, plus `serverTime`/`serverHm`
  (clock re-anchor) and `nextPullSec` (the daemon's half of the power ladder:
  3600 idle / 900 when any session is mid-turn or awaiting).
- `POST /outbox` `{board, decisions[]}` → per-decision results in request
  order; every acknowledged decision is deleted on-device regardless of status
  (`expired`/`rejected`/`unknown_card` are terminal). The daemon validates
  against **live** state: a `permission_decision` applies only while its gate
  is still held; an option decision only while the session is still awaiting
  and the echoed `question` matches its current one.
- Auth: LAN requests carry the pairing token as `?token=` (the same token the
  WS path uses; `/health` exposes it).

The M7 protocol adds daemon card modules (THREAD / PULSE / NUDGE / QUEST)
producing `day`-class cards — schema already reserved.

## Firmware delivery

The device implements **AgentDeck WiFi OTA v1** (`src/agentdeck/ota_ws_receiver.*`):
`agentdeck esp32-ota xteink_x4 --firmware firmware/update.bin` (or `xteink_x3`,
or the device IP) pushes an update over the live dashboard WS — no SD pull, no
File Transfer mode. Chunks stream to an SD cache
(`/.crosspoint/agentdeck-ota.bin`), the image is MD5- and structure-validated,
the end-ack is sent inside the daemon's 30 s budget, and only then does the raw
partition flash + otadata switch + restart run (the Arduino `Update` class is
never used — X4 silicon rejects the patched image through `esp_image_verify`).
A flash failure leaves the running firmware bootable. `device_info` reports
`otaSupported` from the live partition table plus `buildHash` (the version's
trailing git sha) for post-OTA deploy verification. The SD update flow
(Settings → System, and boot recovery) remains as the fallback path.

**When the link is too lossy for OTA**, deliver over the File Transfer web
server instead: `scripts/push-firmware-http.sh <device-ip>` POSTs the image to
the SD root as `update.bin` and verifies the landed size before anyone flashes
it. A single HTTP POST rides TCP retransmits, so it survives packet loss that
makes the chunk/ack OTA protocol time out — which is the state one X3 unit is
in (same room, same AP, X4 fine: unit-specific RF loss, handshakes stalling in
`SYN_RCVD`). The device must be put into File Transfer mode by hand first
(Home → File Transfer → Join a Network, or Create Hotspot for a direct link
that bypasses the router entirely); the script cannot press its buttons.

## Roadmap

1. **M4 (done)** — Card view + direct softkey grammar, fed by session attention.
2. **M5 (done)** — Face shell: boot-to-card setting, background Wi-Fi join,
   connection demoted to a status line; the Face renders in every state.
   **M5.6 (done)** — AgentDeck WiFi OTA v1 client (see "Firmware delivery").
3. **M5.5 (done)** — Deck persistence: card records on SD, render-from-cache at
   boot and on daemon loss, "as of" sync age on the Face.
4. **M6 — Outbox + power ladder (implemented, hardware verification pending)**:
   `actionClass` contract (AgentDeck-side first, then `card_class.h` here),
   HTTP pull sync (`feed_client` + `outbox_store` + endpoint cache), timed
   deep-sleep cadence behind the "AgentDeck battery sync" setting (default
   OFF). Hardware reality checks that reshaped the plan: both boards are
   ESP32-C3 (one binary); the front buttons are ADC ladders so the only wake
   sources are the timer and the power button; the stock sleep path cuts the
   battery latch (GPIO13) and kills the RTC domain, so the cadence uses
   `startTimedDeepSleep` which holds the latch HIGH (higher sleep current —
   hence opt-in). The X3's DS3231 has no alarm wiring and only hour/minute
   registers — it cannot wake the MCU; both boards use the drifty internal
   timer, re-anchored by `serverTime` on every pull. Still to verify on
   hardware: actual latch-held sleep current and a real timer wake.
   Battery-class portability is earned here — do not promise phone-free
   real-time push on battery.

   **Verifying the wake is now possible from the daemon side.** A woken client
   with an empty outbox sends exactly one request and identifies itself in
   none of it, so the cadence used to leave no trace at all. The daemon now
   logs every `GET /feed` with the gap since that client's previous pull,
   compared against the `nextPullSec` it handed out last time (`agentdeck
   devices` → `Card feed`). A gap of ~3600 s after a sleep *is* the timer-wake
   proof, and its signed error is the internal timer's drift. Sleep current
   still needs a meter — that half has no software substitute.
5. **M7 (AgentDeck-side) — card feed protocol (contract + framework landed)**:
   `FeedCard.module` (`ModuleCard`) is the non-session card body — title,
   question, ≤4 context lines, ≤3 choices, because slot 1 of the four buttons
   is always the device's own *Later*. The rule is a clamp at the daemon's
   build chokepoint (`sealModuleCard`), not a convention, and all text is
   trimmed to UTF-8 byte budgets there. `cardId` is `module:<moduleId>:<key>`,
   which is how an outbox `card_choice` recorded hours offline routes back to
   its author.

   **THREAD** ships as the reference producer — `info` class, no choices,
   derived entirely from the live roster (no new state, no persistence, no
   product policy): *"1 of 3 threads need you"* plus a line per thread, with an
   honest `(+n more)` instead of a silent truncation.

   **PULSE / NUDGE / QUEST are deliberately not guessed at.** They are the
   `day` class — answerable offline — and each needs a decision this fork has
   not made yet:

   - what a *nudge* is allowed to ask, and who authored the promise it is
     holding the user to;
   - where a *quest* lives across days, and what closes one;
   - what a *pulse* summarizes, and on whose clock it fires.

   The device grammar is ready for all three; the daemon side needs those
   answers before it grows a producer.

   Shared with InkDeck (always-USB, same panel class — the natural first
   surface for push cards). The X3/X4 re-port is pending: today's firmware
   skips module cards (`applyCardFeed` continues on a null `session`), so the
   contract can land ahead of the client without breaking the deployed X4.
