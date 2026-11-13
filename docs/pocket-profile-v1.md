# Pocket Daily profile v1 — design (2026-09-24)

Status: design accepted by the product owner; implementation in phases below.
Contract between this repository and the companion app (`pocket-daily`).

> **2026-09-25 — provider and monitor retired.** Their only data source, the
> AgentDeck daemon, was removed from the firmware. `provider` and `monitor`
> remain valid IDs: stored profiles and documents naming them still load,
> validate and round-trip unchanged (record encoding is unchanged), but the
> reader no longer lists them in `capabilities.homeItems` and
> `homeRowSources` resolves them to no page. At that time the default Home items became
> `reading`, `study`; the current default is documented below. Weather and today's events on Home and the Daily Brief
> come from the companion app (`POST /api/pocket/v1/glance`,
> `docs/pocket-glance-v1.md`). Statements below about provider cards, the
> monitoring card and the AgentDeck toggles are historical.


## Reading interfaces and explicit views (2026-11-13)

`homeInterfaces: 3` keeps profile schema 1, the 32-byte record, generation checks,
and the four-item limit unchanged. `readingShelf` (ID 11) now shows one large
cover with title, author and progress; side keys cycle within the selected books.
It retains all selected books, up to four. `readingList` (12) uses compact rows;
`reading` remains the compatible single current-book layout. At most one of
these reading layouts is allowed. A new unsaved profile defaults to
`[readingShelf]`; existing saved profiles are not rewritten.

The left front button is labelled **Views**. It opens an explicit chooser for
configured Reading, Calendar, Study and Clock sections, plus Articles. Side
keys move within this chooser and Confirm selects the view. Back cancels it
without changing the current book. Outside the chooser, side keys never cross
from the last book into Calendar or another content type. Confirm opens the
selected book; returning from the reader restores that book's position in a
manually ordered shelf. Home and Sync retain their existing exit routes.
Selection changes do not write SD settings.

`clockCompact` adds an inline clock to the reading layout and omits the
redundant header clock. `nextEvent` also works with weather off: a separate
line shows the first event from the current saved today snapshot, or its empty
state. It is not a full calendar. Weather top/bottom retains the weather and
forecast panel. These combinations support Reading Focus, Book + Clock,
Book + Agenda, Book + Weather, Reading List and Calendar presets in the app.

`calendar` (13) is a separate explicit view. Confirm enters date selection;
side keys move by Gregorian day across month and year boundaries. The selected
day reads the bounded saved agenda from [pocket-agenda-v1.md](pocket-agenda-v1.md).
Downloaded empty dates and unavailable dates remain distinct. Book order is
stored separately under [pocket-reading-shelf-v1.md](pocket-reading-shelf-v1.md).
Actual cached covers resolve either the full cover or the theme-sized thumbnail.

Connected previews use unchanged PDLI schema 5; `homeInterfaces: 3` signals the
new presentation and navigation semantics, not a new binary layout. Host
sample previews use original geometric demo covers only; connected snapshots
never invent cover artwork. Native fixtures cover both panel sizes in all four
orientations. Physical button and installed-frame validation are separate
hardware evidence recorded by the developer runner.

## Desk Clock Home extension (2026-11-10)

Clock IDs are advertised in `capabilities.homeItems`. The existing profile
generation and record fields carry them. When `clockCompact` is advertised,
at most one full-screen clock can be placed in the Home page order alongside
reading and cards. A previously saved single-clock layout still loads.
Earlier firmware that advertises only the portrait clock IDs requires a
full-screen clock to be its sole Home item.

- `clockEvent` shows the earliest saved timed event that has not started
  and the minutes until it starts.
- `clockWeather` shows a saved rain window or a compact weather summary.
- `clockLine` shows a short offline prompt selected by local calendar day.
- `clockLandscape` rotates the full-screen next-event clock clockwise. It
  restores the previous renderer orientation when leaving the page.
- `clockCompact` draws HH:MM above the selected reading layout. It requires
  one reading layout, adds no separate page and updates only at minute boundaries.

Desk Clock follows the companion time and offset from the Sync clock
endpoint when available. Without that offset, it uses the saved glance's
offset or the reader's configured timezone. An unset clock shows dashes.
In production, a visible full-screen clock toggles the colon every two seconds and paints one
folded intermediate frame at consecutive minute changes. Confirm resumes the book
or opens Library. Back, Articles and Sync keep their existing actions.
Continue Reading remains the default Home. The clock has no deep-sleep
refresh and does not change the Sleep profile.
Developer firmware currently displays HH:MM:SS with steady colons and one
folded frame per consecutive second for the physical panel trial. The saved
profile and app preview continue to use the same format.

Older readers do not advertise the IDs. Older companion apps reject a
profile containing unknown IDs instead of rewriting it.

## Why

The companion should configure Pocket Daily itself — which Home items appear,
in what order, and what the sleep frame shows — without reflashing. Before this
contract only theme metrics (UI packs, removed 2026-10-01) and four preferences
were data-driven; Home composition and feature behaviour were C++ in
`PocketDailyActivity`.

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
    "items": ["reading", "study"],
    "dailyWord": false,
    "weather": "off",
    "nextEvent": false
  },
  "sleep": {
    "mode": "brief",
    "sections": ["reading"]
  }
}
```

- `home.items`: ordered, distinct subset of `reading`, `study`, `word` (and the
  retired `provider`, `monitor`, which are accepted but never shown); at least
  one, at most four. Reading keeps an empty Library entry point without a
  book; optional card sources without data are skipped.
- `study` is the companion's own cards ("My cards", up to three pages, each
  drawn with its image on Home). Without a `word` item it falls back to the
  daily word when there are no cards and `dailyWord` is true (the v1
  behaviour). `word` (added 2026-09-25, additive) is the daily word from the
  built-in list or an SD learning pack as its own page; with it, `study`
  never repeats the word. `PocketDaily::Home::homeRowSources` holds these rules
  for the device and the host preview.
- `weather`: `bottom` (today), `top` or `off` (main panel takes the space).
- `sleep.mode`: `brief` (Daily Brief) or `reader` (the upstream sleep screen
  chosen in the reader's own Sleep Screen setting).
- `sleep.sections`: ordered distinct subset of `reading`, `study`, `weather`,
  `today`, `card`; `study` keeps today's rule of appearing only without an open
  book while `reading` is also selected (it always appears when `reading` is
  not). `card` (added 2026-09-25, additive) is the first of My cards with its
  image, shown whether or not a book is open, for example contact details or
  a QR code for a lost reader.
- A sleep section that cannot fit its header and first line is skipped rather
  than drawn over the status line.
- Readers advertise the IDs they offer in `capabilities` (`homeItems` is
  `reading`, `study`, `word` since 2026-09-25); the companion offers only
  those, and refuses a stored document with IDs it does not know.
- Sleep frame: weather fills to its bottom bound only when it is the last
  section; otherwise it gets Home's fixed 272px panel. Reading's cover shrinks
  to the space left and falls back to the compact text form below 108px.
- Quick-resume wake draws a full-width band (power glyph, "Waking up", dots)
  over the restored frame instead of the corner loading icon that covered the
  Daily Brief status line. Built-in fonts are ASCII only, so a non-ASCII
  translation falls back to English on this path.
- Home shows at most `maxHomeItems` (4, the existing `kOverviewCap`) items;
  when the ordered sources produce more, later ones are dropped.
- A reader with no stored profile uses the reading-first defaults above
  (2026-10-13). Existing profiles retain their choices. Sleep Study uses own
  cards, then vocabulary only when `dailyWord` is enabled; a standalone Home
  Word page does not silently enable that fallback. Selected weather without
  saved data explains how to configure and sync it from the app.
- Unknown keys, unknown IDs, duplicates, wrong types or an empty item list
  reject the whole document (400). Nothing is applied partially.

## Device storage and application

- A fixed binary struct (< 32B) lives in RAM, loaded once at boot
  (`ProductBoot::loadPersistedState`). No SD parse or allocation on the Home
  paint or sleep path.
- Persisted as two alternating slots `/.crosspoint/pocket-profile.{0,1}.bin`
  (magic, schema, generation, payload, CRC32). A write goes to the inactive
  slot and is read back before RAM changes; the newest valid slot wins at
  boot; no valid slot means defaults.
- Pocket Daily reads the RAM profile in `collectOverview`, the Home layout and
  the sleep frame. A changed profile takes effect the next time Pocket Daily
  paints (entering it, or on return from a Sync session).
  Drawing the saved Home or Daily Brief inside a Sync session is proposed in
  [pocket-screen-present-v1.md](pocket-screen-present-v1.md) (2026-09-26).

## Endpoints (Sync profiles, identity bound)

- `GET /api/pocket/v1/profile?deviceID=` → the document plus `generation`
  and the accepted IDs (`capabilities`) so the app never guesses.
- `POST /api/pocket/v1/profile?deviceID=&generation=<expected>` with the full
  document → validate, persist, then 200 with the stored document and new
  generation. 409 identity or generation mismatch, 400 invalid, 503 when heap
  is below the existing content-operation floor.
- `GET /api/pocket/v1/display?deviceID=` → resolved render inputs: theme,
  orientation, font family and size, content-page padding/spacing from
  the theme metrics, and the localized, button-remapped strings the reader
  would draw.
- Implementation: `PocketProfile.{h,cpp}` (model, strict ArduinoJson parser,
  response writer, 32-byte record), `PocketProfileStore.{h,cpp}` (slots,
  boot load in `ProductBoot::loadPersistedState`), `GET/POST
  /api/pocket/v1/profile` on both Sync profiles, `/api/status`
  `pocketProfile: 1`. Host tests fetch ArduinoJson 7.4.2 like the firmware.
- `display` is registered with content presentation; the app reads it once
  per connection, inside its sequential reader lane. A 404 (older firmware) means the labelled reference preview. The
  profile endpoints will be advertised in `/api/status` when they land.
- The developer `dev/capture` and `dev/frame` routes used for the P1-1
  host/device parity check were removed 2026-10-01 with live frame capture;
  a new parity check needs another way to read the device frame.

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

Host preview of My cards: `pdui_set_cards` passes the companion's PDCT cards
and PBM images (validated like `pdui_render_content`) so the Home and Daily
Brief previews show them instead of the sample study card. Card images are
shrunk to fit, never enlarged (`fitContentImage`), so the companion sizes QR
codes itself (about 240 px, whole-pixel modules).

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
