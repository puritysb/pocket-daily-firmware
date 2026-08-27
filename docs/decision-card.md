# Offline-first Pocket Reader — product contract

This fork turns XTeink X3/X4 into an **offline-first Pocket reader**. It is not
an AgentDeck peripheral and not a live-session dashboard. CrossPoint remains the
device OS and EPUB engine; AgentDeck is one invisible background source that
prepares small, adaptive items while connectivity exists.

## Product promise

The useful state is the disconnected state:

1. ordinary boot always lands on Pocket;
2. the open EPUB and its last position are local and appear first;
3. daemon-authored items are copied to SD and remain readable offline;
4. every choice, including **Later/Done**, is written to the SD Outbox before
   the item leaves the screen;
5. Wi-Fi and daemon discovery never replace local content with an error screen;
6. the retained sleep frame shows only durable personal information: reading,
   weather and today's schedule.

Holding **Back** during boot opens the full book library. Holding **OK** resumes
the current book. Those are escape paths from Pocket, not alternate product
identities.

## Information architecture

Pocket Home is a bounded physical list:

1. **Continue Reading** — current local EPUB, when present;
2. **Pocket items** — at most three daemon-authored cards, ordered by the
   adaptive feed;
3. **Personal Glance** — reading/weather/today when there are no queued items.

Live AgentDeck sessions, provider quotas, terminal prompts and AgentDeck
branding are forbidden on Pocket Home. Sessions may influence daemon-authored
NUDGE/PULSE/QUEST items, but they never become rows or THREAD digests themselves.

## Offline data lifecycle

- `/.crosspoint/pocket-daily-deck.bin` stores the bounded Pocket pool, glance and
  feed signature using tmp+rename.
- `/.crosspoint/pocket-daily-outbox.bin` stores choices before UI removal.
- Existing `agentdeck-*.bin` files are imported once and removed only after the
  Pocket Daily copy has been written successfully.
- The device requests the legacy `GET /feed?surface=pocket-reader` projection
  while its Surface v1 headers declare `portable-reader/v1`, which omits session
  projections while retaining daemon-authored module cards.
- `choiceId: later` is a neutral read-state. The daemon marks that exact card as
  handled without rewarding or penalising its content category, so it does not
  immediately return after the Outbox is drained.
- The daemon keeps the learning model; the device keeps only bounded content
  and pending actions. No on-device LLM or unbounded history is implied.

## Four-button grammar

Pocket items keep one-screen interaction:

| Slot | Read-only item | Item with choices |
| --- | --- | --- |
| 1 | Done | Later |
| 2 | — / choice 1 | choice 1 |
| 3 | — / choice 2 | choice 2 |
| 4 | — / choice 3 | choice 3 |

Pocket Home uses Back for **Library**, OK for **Read/Open/Sync**, and the side
buttons for navigation. A missing network never disables an already-carried
item.

## Power and sync

New installs default Pocket background sync on. A timer wake joins the last
known Wi-Fi, pushes the Outbox, pulls the bounded feed, paints once, and returns
to timed deep sleep. An unavailable network consumes only the bounded wake
budget and leaves the saved Pocket intact. The physical power button remains
the reliable wake source; the front-button ADC ladder cannot wake the ESP32-C3.

## Memory contract

X3/X4 are no-PSRAM ESP32-C3 devices. Pocket therefore uses one local reading
row, a fixed three-card pool, fixed UTF-8 buffers and an eight-record Outbox.
Recommendation policy and history stay in the daemon. UI code must not add
unbounded containers, per-frame heap growth, or new large stack objects.

The render task has a 12 KB stack. Pocket's two four-row scratch buffers and
the glance snapshot live once inside the heap-allocated activity object, with
separate render/loop copies to avoid cross-task races. They must never move
back into `render()` locals: the former 8 KB stack overflowed on X3 at boot.

## Delivery

Every successful `pio run -e default` stages `firmware/update.bin`. Read
`firmware/LATEST_BUILD.txt` before claiming installation. X3's reliable fallback
is SD recovery: copy `update.bin` to the SD root, then boot with **UP + POWER**.
