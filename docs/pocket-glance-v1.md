# Pocket glance v1 — app-provided weather and events (2026-09-25)

Contract between this repository and the companion app (`pocket-daily`,
`Sources/Glance/ReaderGlance.swift`). Pocket Daily shows weather and today's
calendar events on Home and the Daily Brief; the companion composes them on the
phone or Mac (Apple Weather, the user's calendars) and sends them over the
existing Pocket Sync HTTP server. The reader never fetches them itself and has
no other source for them: the AgentDeck daemon path that used to supply the
glance was removed (see "Retired" below).

## Endpoint

`POST /api/pocket/v1/glance?deviceID=<8 upper hex>`

- Registered on both Sync profiles, next to `/api/pocket/v1/profile`, and
  advertised as `"pocketGlance": 1` in `/api/status` on those profiles.
- Same identity and heap admission as the profile endpoints
  (`admitContentOperation`): 409 while another transfer is active or when the
  `deviceID` does not match, 503 below the content-operation heap floor.
- Body: the JSON document below, at most 2048 bytes (any content type; the app
  sends `text/plain`).
- `200 {"schema":1,"deviceID":"5B09AF70","savedEpoch":1790350200}` once the
  glance is validated and stored.
- `400 <short reason>` for any invalid document; nothing is stored or changed.
- `503` when the reader lacks memory to parse or store it; `500` when the SD
  write fails — the previous glance stays in use in both cases.
- `GET` is not implemented (404/405).

## Document (schema 1)

```json
{
  "schema": 1,
  "savedEpoch": 1790350200,
  "syncedHm": "00:30",
  "utcOffsetMinutes": 540,
  "weather": {
    "place": "Seoul",
    "code": 61,
    "tempC": 18,
    "summary": "Rain",
    "todayMinC": 12, "todayMaxC": 21,
    "rainStartHm": "14:00", "rainEndHm": "17:00",
    "rainProbability": 80,
    "days": [
      {"date": "2026-09-26", "summary": "Rain", "code": 61, "minC": 12, "maxC": 21, "rainProbability": 80}
    ]
  },
  "events": [
    {"startHm": "09:30", "endHm": "10:00", "title": "Team standup"},
    {"startHm": "", "endHm": "", "title": "All-day item"}
  ]
}
```

Every key shown is required; `weather` may be `null`. Parsing is strict
(ArduinoJson 7, `PocketGlanceJson.cpp`): an unknown or missing key, a wrong
type (a float where an integer is required, a string number, …), an
out-of-range value, an oversize string or `schema != 1` rejects the whole
document.

| Field | Rule |
| --- | --- |
| `savedEpoch` | integer unix seconds when the app composed it, `>= 1700000000`, fits uint32 |
| `syncedHm` | `"HH:MM"` (00:00–23:59), the app's local compose time |
| `utcOffsetMinutes` | integer −720..840, the app's UTC offset at `savedEpoch` |
| `weather.place` | string, ≤ 23 UTF-8 bytes (may be empty) |
| `weather.code`, `days[].code` | integer WMO weather code −1..99 (−1 unknown) |
| `weather.tempC`, `todayMinC`, `todayMaxC`, `days[].minC`, `days[].maxC` | integer −100..100 °C |
| `weather.summary`, `days[].summary` | string, ≤ 11 bytes (may be empty) |
| `rainStartHm`, `rainEndHm`, `events[].startHm`, `events[].endHm` | `""` or `"HH:MM"` |
| `rainProbability`, `days[].rainProbability` | integer −1..100 (−1 unknown) |
| `weather.days` | 0..5 entries, first = the app's today; `date` is `"YYYY-MM-DD"` |
| `events` | 0..3 entries; `title` is a non-empty string ≤ 48 bytes |

All strings must be valid UTF-8 without NUL, C0 or C1 control characters
(newline and tab included; shared `Text::validUtf8`). The app must shorten
titles on a character boundary and replace line breaks before sending.

Mapping into `PocketDaily::Glance`: `weather.valid` is true when `weather` is
an object; `weather.tomorrow` is `days[1]` when present; `glance.valid` is true
when weather or at least one event is present. Usage and wrap-up fields are
never filled. `weather: null` replaces any earlier weather — the reader does not
merge documents.

## Storage

- One 404-byte record `/.crosspoint/pocket-glance.bin` ("PDGL", version 1,
  little-endian fixed layout independent of struct padding, unused slots zero,
  CRC-32/ISO-HDLC). Decoding re-applies every field rule above, so a corrupt or
  hand-edited file fails closed to "no glance".
- Save: write `pocket-glance.tmp`, read it back and compare byte for byte, move
  the active file to `pocket-glance.bak`, rename the temp file into place,
  verify it again, then delete the backup. Any failure restores the previous
  file; a load falls back to the backup if power was lost between the renames.
- Nothing is kept in RAM by the store. Pocket Daily loads the record once when
  it is entered, into its own heap-allocated activity (~800 B, freed on exit);
  there is no static buffer and no SD access on the paint path. A glance posted
  during a Sync session is shown the next time Pocket Daily is entered.

## Clock and local time

The reader has no network clock of its own (the daemon's SNTP and clock
estimate went with the daemon path; the X3 DS3231 is not used here).

- On a successful POST, if the system clock is unset (`time() < 1700000000`,
  i.e. after a cold boot), it is set to `savedEpoch` with `settimeofday`. A set
  clock is never moved. `savedEpoch` is a lower bound, seconds to minutes
  behind real time.
- Where the clock is still unset (for example after a later reboot without a
  new glance), the daily word uses `savedEpoch` as its date; age and staleness
  labels need a set clock and are simply omitted.
- `utcOffsetMinutes` is stored with the glance and is the only timezone the
  reader knows. The daily word turns over at the app's local midnight
  (`(epoch + offset·60) / 86400`, UTC midnight when there is no glance).
- When the local date (clock + offset) is later than the local date of
  `savedEpoch`, Home and the Daily Brief drop that day's events and every
  forecast day before today; the "now" block is replaced by today's forecast
  day (no current temperature or rain window), and a forecast with no day left
  is not drawn. `GlanceFormat::rollToLocalDay`, host-tested.
- The Home status line reads `WI-FI OFF / SYNC · SAVED · <age>` (age from
  `savedEpoch`, only with a set clock); the ambient Daily Brief header shows
  the forecast date and `SYNC HH:MM` (`SAVED HH:MM` after 36 hours). The
  powered-off frame shows neither.

## Retired: AgentDeck daemon glance

Before 2026-09-25 the glance came from the AgentDeck daemon's `GET /feed`
(`src/agentdeck/protocol.cpp`), was cached with the provider cards in
`/.crosspoint/pocket-daily-deck.{0,1}.bin` (`docs/deck-cache-v7.md`) and also
carried provider usage and a work wrap-up. That path, its settings
(`agentDeckCompanionEnabled`, `agentPullSyncEnabled`), the timed-wake pull
cadence and all of `src/agentdeck/` were deleted. Old deck, outbox, endpoint
and auth files on SD are no longer read; they are harmless and can be deleted.

## Verification

Host: `test/pocket_glance` (parser: valid, boundary, malformed, unknown keys,
oversize strings, too many days/events, schema; record round trip and
corruption; store: atomic save, reload, failed writes keep the previous
glance, backup fallback; local-day roll), `scripts/test_sync_routes.py`
(route admission order, status flag, load before first paint). Not verified on
hardware yet: an app POST over Nearby Sync, the clock set on a cold-booted
reader, and whether the set clock survives the silent restart back to Pocket
Daily.
