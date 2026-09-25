# Pocket Daily profile v1 — design (2026-09-24)

Status: design accepted by the product owner; implementation in phases below.
Contract between this repository and the companion app (`pocket-daily`).

## Why

The companion should configure Pocket Daily itself — which Home items appear,
in what order, and what the sleep frame shows — without reflashing. Today only
theme metrics (63 `.uipack` fields) and four preferences are data-driven; Home
composition and feature behaviour are C++ in `PocketDailyActivity`.

## P0 inventory (firmware 77953e61)

Home (`renderOverview`) is one fixed screen, not a list of sections:

| Region | Today |
| --- | --- |
| Header, status band | Always; status carries network truth (keep fixed) |
| Main panel | Pages through one item list built only in `collectOverview`: Continue Reading (open EPUB), then app cards if active else the built-in daily word, then cached provider cards. Cap `kOverviewCap = 4`; cursor is an index |
| Weather + 5-day grid + next event | Always drawn below the main panel (250-272px) |
| Chevrons, 4-slot button strip | Fixed per device; raw button indices |

Sleep: while Pocket Daily is current, `paintSleepFrame` paints its own
powered-off Daily Brief (Reading, Study only without a book, Weather, Today) and
upstream `SleepActivity` modes never run. `pocketDailySleepCover` only toggles
the cover. Monitoring data (`Glance.usage`, `wrapup`) is parsed but never drawn.

Existing toggles hide no Home region: `agentDeckCompanionEnabled` gates the
radio only (cached provider cards still show), `agentPullSyncEnabled`,
`pocketDailySleepCover`, `startupApp`.

Known drift found (not changed by this design; the preferences partial-apply
issue was fixed on 2026-09-25): decision-card says background
sync defaults on (code: off), OK is Sync (code: Right), boot always lands on
Pocket (code honours `startupApp`); a ~6.2KB deck cache is allocated on every
entry even with AgentDeck off; `buildLocalStudyCard` keeps a 928B record on the
stack; preferences POST can partially apply; `settings.json` is not written
atomically.

The app's card preview used Base metrics (spacing 10) while the default device
theme is Lyra (spacing 16), so the "exact" canvas differed on default readers.

## Decisions

1. Relax the Home rule: a **read-only AgentDeck monitoring card** (provider
   usage rows and wrap-up lines) may be one Home item when the profile enables
   it. Live sessions, terminal prompts and actionable session buttons remain
   forbidden; the attention-steering invariant in `src/agentdeck/AGENTS.md`
   is unchanged.
2. Profile v1 scope: Home item order/visibility, study fallback, weather panel
   position/off, next-event line; sleep mode and Daily Brief section
   order/visibility. Status band and button strip stay fixed.
3. The app reads the reader's resolved display state and renders previews with
   it, instead of assuming a reference theme.

## Profile document (HTTP JSON, schema 1)

```json
{
  "schema": 1,
  "home": {
    "items": ["reading", "study", "provider"],
    "dailyWord": true,
    "weather": "bottom",
    "nextEvent": true
  },
  "sleep": {
    "mode": "brief",
    "sections": ["reading", "study", "weather", "today"]
  }
}
```

- `home.items`: ordered, distinct subset of `reading`, `study`, `provider`,
  `monitor`; at least one. An item with no data is skipped as today (no book,
  no provider cards, no usage data).
- `study`: active app cards; when none, the built-in daily word if
  `dailyWord` is true.
- `weather`: `bottom` (today), `top` or `off` (main panel takes the space).
- `sleep.mode`: `brief` (Daily Brief) or `reader` (the upstream sleep screen
  chosen in the reader's own Sleep Screen setting).
- `sleep.sections`: ordered distinct subset of `reading`, `study`, `weather`,
  `today`; `study` keeps today's rule of appearing only without an open book
  while `reading` is also selected (it always appears when `reading` is not).
- Sleep frame: weather fills to its bottom bound only when it is the last
  section; otherwise it gets Home's fixed 272px panel. Reading's cover shrinks
  to the space left and falls back to the compact text form below 108px.
- Quick-resume wake draws a full-width band (power glyph, "Waking up", dots)
  over the restored frame instead of the corner loading icon that covered the
  Daily Brief status line. Built-in fonts are ASCII only, so a non-ASCII
  translation falls back to English on this path.
- Home shows at most `maxHomeItems` (4, the existing `kOverviewCap`) items;
  when the ordered sources produce more, later ones are dropped. `monitor`
  appears only when carried usage or wrap-up data exists; OK does nothing on
  it (read-only) and it shows the snapshot's absolute sync time.
- The defaults above reproduce today's behaviour exactly; a reader with no
  stored profile behaves as before.
- Unknown keys, unknown IDs, duplicates, wrong types or an empty item list
  reject the whole document (400). Nothing is applied partially.

## Device storage and application

- A fixed binary struct (< 32B) lives in RAM, loaded once at boot next to the
  startup UI pack. No SD parse or allocation on the Home paint or sleep path.
- Persisted as two alternating slots `/.crosspoint/pocket-profile.{0,1}.bin`
  (magic, schema, generation, payload, CRC32). A write goes to the inactive
  slot and is read back before RAM changes; the newest valid slot wins at
  boot; no valid slot means defaults.
- Pocket Daily reads the RAM profile in `collectOverview`, the Home layout and
  the sleep frame. A changed profile takes effect the next time Pocket Daily
  paints (entering it, or on return from a Sync session).

## Endpoints (Sync profiles, identity bound)

- `GET /api/pocket/v1/profile?deviceID=` → the document plus `generation`
  and the accepted IDs (`capabilities`) so the app never guesses.
- `POST /api/pocket/v1/profile?deviceID=&generation=<expected>` with the full
  document → validate, persist, then 200 with the stored document and new
  generation. 409 identity or generation mismatch, 400 invalid, 503 when heap
  is below the existing content-operation floor.
- `GET /api/pocket/v1/display?deviceID=` → resolved render inputs: theme,
  orientation, active UI pack, content-page padding/spacing from the effective
  metrics, and the localized, button-remapped strings the reader would draw.
- Implementation: `PocketProfile.{h,cpp}` (model, strict ArduinoJson parser,
  response writer, 32-byte record), `PocketProfileStore.{h,cpp}` (slots,
  boot load in `ProductBoot::applyStartupUiPack`), `GET/POST
  /api/pocket/v1/profile` on both Sync profiles, `/api/status`
  `pocketProfile: 1`. Host tests fetch ArduinoJson 7.4.2 like the firmware.
- `display` is registered with content presentation; the app reads it once
  per connection and after a UI pack apply/revert, inside its sequential reader
  lane. A 404 (older firmware) means the labelled reference preview. The
  profile endpoints will be advertised in `/api/status` when they land.
- Developer builds only: `POST /api/pocket/v1/dev/capture` stores the
  completed content frame (409 unless the presentation receipt is rendered)
  and `GET /api/pocket/v1/dev/frame?offset=` reads it in chunks, for the
  host/device parity check (`MacTests/PocketParityTests.swift` in the app).

P1-1 device finding (X3 wbfc03fa5): the tested reader runs the **classic**
theme (spacing 10) and draws "« Back", while the old app preview assumed Base
strings "Back" and the new fallback assumes Lyra. Neither assumption holds for
every reader, which is why previews use the resolved display state.

## Phases and verification

| Phase | Scope | Acceptance |
| --- | --- | --- |
| P1-1 | `display` endpoint; app renders card previews from it (Lyra default when unavailable, labelled) | **Met 2026-09-25** on X3 wbfc03fa5: gen7 card with image, classic theme, 0 of 418,176 pixels differ; replacing "« Back" with "Back" alone yields 600 (control) |
| P1-2 | Profile store, endpoints, Home/sleep application, monitoring card | Host tests (11) and source checks pass; device w15cb1e54: GET defaults at generation 0, invalid 400, stale generation 409, identity 409, valid save generation 1 read back and kept across reboots. User confirmed on X3: Home order and weather-top layout; sleep frame after the w1e22398f fix (weather capped at 272px when sections follow, reading cover shrinks to the room left) |
| P1-3 | Extract a pure Home/sleep renderer shared by device and host; host ABI for Home preview | **Pipeline done 2026-09-25, tuning pending**: `src/pocket_daily/home/` (HomeRenderer, HomeDrawing) draws Home and the Daily Brief on both targets; `pdui_render_home`/`pdui_render_brief` (ABI 1, additive) with a `pdui_profile` and `PDUI_SAMPLE_*` sample mask; 14 host tests (determinism, placement, order, selection, empty samples, invalid profiles on 792×528 and 800×480). Remaining: device capture comparison with pinned clock/battery, then golden hashes |
| P2 | App studio edits the profile on the Home canvas and sends it | UI tests; physical acceptance |

P1-3 host stand-ins and known tuning items (compare against a reader capture):
the host header is a plain title and rule, not the device theme header; the
book cover is a hatched stand-in; host fonts are the built-in UI fonts plus
the cpfont for CJK only; some layouts leave the bottom weather panel's current
temperature area empty (same code on the device). The device overview and
the host preview resolve Home items to row sources with the shared
`homeRowSources` (order, daily-word fallback, items without content skipped),
so only the row contents differ. Moving
the renderer also made the Daily Brief skip Today when it would reach the
status line, which changes the device frame too.

Memory rule for every phase: no new resident heap on Home, Sync or the sleep
path; request-time buffers stay bounded and behind the existing heap gates.
