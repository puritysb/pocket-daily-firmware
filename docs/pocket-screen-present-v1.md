# Home and Sleep presentation in Sync — contract v1 (proposed 2026-09-26)

Status: **proposed, not implemented.** Contract between this repository and the
companion app (`pocket-daily`). The app ships without it and detects it only
through the status flag below. Nothing here changes existing routes.

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
the routes.

- `POST /api/pocket/v1/screen/present?deviceID=<8 upper hex>&surface=<home|brief>&generation=<n>`
  - `generation` is the stored profile generation the app just saved (or read).
    A different stored generation or device ID returns 409. A generation with a
    `sleep.mode` of `reader` returns 409 for `brief`, because there is no Daily
    Brief to draw.
  - Validation and queueing only: the handler copies surface and generation
    (a few bytes) into the activity-owned request and returns 202 with the
    receipt. No SD read, font load or allocation happens in the HTTP callback.
  - 503 when the request cannot be queued (another writer or presentation
    owns the display). Nothing is retried by the reader.
- `GET /api/pocket/v1/screen/presentation?deviceID=<id>&surface=<home|brief>`
  - Read-only receipt, same rules as the content receipt: schema 1, `deviceID`,
    `surface`, `generation`, `phase` (`queued`, `rendered`, `failed`), optional
    `failure` (`none`, `memory`, `preparation`, `display`), `heap`, `block`.
  - Answers without SD access or the render lock, so a lost POST response is
    resolved by reads only.

## Drawing

- One presentation slot shared with content presentation. A newer request of
  either kind replaces a queued one; only the latest request has a receipt.
- Preparation runs after HTTP client cleanup on the main task under the
  activity's RenderLock, exactly where `ContentPresentation::service` runs, and
  samples the existing cold admission gates (16 KiB free, 4 KiB largest block)
  outside the request. The gates are not lowered for this feature.
- Inputs, all bounded and released after the paint:
  - the RAM profile (already resident, < 32 B);
  - the stored app glance, decoded from its 404 B record into activity-owned
    storage for the duration of the paint;
  - the active My cards snapshot through `ContentViewState` (<= 2400 B, the
    same allocation content presentation uses);
  - the open book's title, author and percent from existing state. No cover:
    Home uses its text form and the Daily Brief the compact reading section,
    so no image decode joins the Sync memory budget.
- Fonts come from the `BoundedUI` facade with coverage checked before the
  framebuffer is cleared, as for cards. Missing coverage is a `preparation`
  failure, never a partial frame.
- The frame is `PocketDaily::Home::renderHome` or `renderBrief` from
  `src/pocket_daily/home/`, so it matches the companion's host preview. The
  Daily Brief is drawn as a preview: its bottom status line reads that it is a
  sleep-screen preview and that Back returns to Sync, so the panel never
  claims the reader is powered off while its radio is on.
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
