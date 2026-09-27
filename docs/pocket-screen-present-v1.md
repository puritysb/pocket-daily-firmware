# Home and Sleep presentation in Sync — contract v1

Status: **implemented locally 2026-09-26; hardware acceptance pending.** Revised after
tracing the device renderer (cover column, daily word, fonts). Contract between this repository and the
companion app (`pocket-daily`). The app ships without it and detects it only
through the status flag below. Nothing here changes existing routes.
Local checks and remaining physical acceptance are recorded in
[`SCREEN_PRESENTATION_HANDOFF.md`](SCREEN_PRESENTATION_HANDOFF.md).

## Why

The companion's Apply saves the profile (`docs/pocket-profile-v1.md`), reader
preferences and My cards while the reader stays in Sync. Cards can already be
shown on the panel without leaving Sync (`docs/content-display.md`). A saved
profile takes effect only when Pocket Daily next paints, which is after the user
leaves Sync, so the user edits Home or Sleep without seeing the reader's own
frame until the session ends. This contract lets the app ask the reader to draw
the saved Home or Daily Brief inside the Sync session, like a card page.

## Route

Registered on both Sync profiles next to content presentation, only when the
transfer activity registers the host callbacks. `/api/status` then reports
`screenPresentation: 1`; older firmware omits it, and the app must not call
the routes. Both routes check `deviceID` first (409 on mismatch), accept only
`surface=home` or `surface=brief` and a decimal `generation` (400 otherwise).

- `POST /api/pocket/v1/screen/present?deviceID=<8 upper hex>&surface=<home|brief>&generation=<n>`
  - `generation` is the stored profile generation the app just saved (or
    read); a different stored generation returns 409, and so does `brief`
    while `sleep.mode` is `reader` (there is no Daily Brief to draw).
  - Admission as for content (`admitContentOperation`): 409 while an upload
    owns the reader, 503 when memory is below the request floor or the slot
    cannot be queued. The handler copies surface and generation into the
    activity-owned request and answers 200 with the receipt (`phase: queued`).
    No SD read, font load or allocation happens in the HTTP callback.
- `GET /api/pocket/v1/screen/presentation?deviceID=<id>&surface=<home|brief>&generation=<n>`
  - Read-only receipt, answered without SD access or the render lock, so a
    lost POST response is resolved by reads only. 409 when that surface and
    generation are not the current request (for example, a newer card or
    screen request replaced it).
- Receipt (both routes, 200):
  `{"schema":1,"deviceID":"5B09AF70","surface":"home","generation":7,"phase":"queued","failure":"none","heap":0,"block":0}`
  with `phase` one of `queued`, `rendered`, `failed` and `failure` one of
  `none`, `memory`, `preparation`, `display`; `heap` and `block` are the
  deferred admission sample (0 before it).

## Drawing

- One presentation slot shared with content presentation. Queued or drawing
  requests own the slot and reject another request with 503. Once completed
  (rendered or failed), a newer accepted request of either kind replaces it;
  only that latest request has a receipt. This preserves the existing content
  writer gate instead of interrupting a prepared frame or its fonts.
- Preparation runs after HTTP client cleanup on the main task under the
  activity's RenderLock, exactly where `ContentPresentation::service` runs, and
  samples the existing cold admission gates (16 KiB free, 4 KiB largest block)
  outside the request. The gates are not lowered for this feature.
- Inputs, all bounded and released after the paint (one per-paint bundle,
  never activity members):
  - the RAM profile (already resident, < 32 B), rechecked against the
    requested generation at preparation;
  - the stored app glance (`AppGlance::load`, ~788 B), rolled to today like
    the Pocket activity does;
  - the active My cards snapshot through `ContentViewState` (<= 2400 B, the
    same allocation content presentation uses); card images are drawn with the
    existing PBM path (no heap, <= 64 B row scratch);
  - the open book's title and author from `RECENT_BOOKS` and its percent from
    the 7-byte `progress.bin` read;
  - the daily word from the SD learning pack when it is the drawn page: one
    open, the pack header (shape and checksum, not the whole-pack SHA) and
    one record, freed before fonts load; the built-in list only when the pack
    is missing or unreadable, as on the device.
- Covers are never decoded. Home always reserves its cover column and the
  Daily Brief keeps its cover layout when `pocketDailySleepCover` is on, so
  both draw the device's own placeholder cover art (frame, spine and lines;
  pure drawing, no SD). This is the one intended difference from the frame
  the reader shows after Sync.
- Fonts: text without Hangul, Kana or Han uses the built-in flash fonts. When
  any drawn string needs CJK, the `BoundedUI` PocketSansWorld family is loaded
  once and every drawn string, the header title and "…" are checked in regular
  and bold before the framebuffer is cleared; missing coverage is a
  `preparation` failure. The `Cached` UiCjkFont path is never used here.
- The frame is `PocketDaily::Home::renderHome` or `renderBrief` from
  `src/pocket_daily/home/`, with rows from the shared `homeRowSources`, so it
  matches the companion's host preview. It is painted in portrait (the
  orientation Pocket Daily uses) and the previous orientation is restored.
  The Daily Brief is drawn as a sleep frame without the wake cue; its bottom
  status line says it is a sleep-screen preview and that Back returns to Sync,
  so the panel never claims the reader is powered off while its radio is on.
- Back returns to the transfer view without ending the session; RSSI updates
  do not redraw the presented frame. No navigation within the frame.
- No new task, server, framebuffer, resident buffer, radio change, reboot or
  Wi-Fi reassociation. A failed paint emits a `failed` receipt, keeps the
  last good panel frame and does not retry.

`rendered` means the display driver call returned, not optical proof.

## Companion behaviour

- After an Apply that saved the profile, the app asks for the surface the user
  is editing (Home, or the Daily Brief when that is the sleep mode), then
  follows the receipt with reads only. Its status says the screen is shown
  only after `rendered` for that generation.
- Without `screenPresentation` the app says Home and Sleep show when the user
  leaves Sync. Card pages keep using content presentation.
- Button and reading preferences need no presentation: the reader applies
  them immediately (`docs/nearby-sync-v1.md`, companion preferences).

## Acceptance before the app relies on it

- Host tests: controller transitions, generation and identity checks, the
  shared slot with content presentation, fault injection for load, memory,
  font coverage and paint failures (no display call on failure, no retry,
  fonts and writer gate released).
- X3 on same Wi-Fi: three consecutive Applies in one session each reach
  `rendered` with the heap and block samples recorded, Back returns to Sync,
  and a card presentation still succeeds afterwards; then a 30-minute idle
  without a lasting drop in free heap. X4 and direct connection separately.
