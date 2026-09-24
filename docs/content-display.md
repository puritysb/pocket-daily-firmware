# Stored app cards in the Pocket reader

## Connection-preserving presentation boundary

Content presentation must not call the existing File Transfer exit path:
`CrossPointWebServerActivity::onExit` restarts after an active Wi-Fi session,
and the Nearby variant restarts before radio teardown. Merely extending
`session/end` to STA would therefore reintroduce reconnects on every edit.
Those legacy exit paths are unchanged here.

The local implementation hosts a lightweight content view within the selected
transfer session, using the existing server, renderer, HAL and framebuffer.
Changing the visible view must not end the transport session, restart, associate
Wi-Fi again, or instantiate the full Pocket activity alongside the live server.
Session termination remains a separate explicit action. This integration is
not yet physically validated.

`ContentViewState` now separates verified display data from Pocket activity
and network lifetimes. The existing offline Pocket activity uses it. It owns
one on-demand <=2400B card snapshot, reuses that allocation on reload, and pins
the active revision plus generation only after complete verification. Empty
content is distinguishable from missing/unreadable active state. Failure clears
the model; no partial or stale cards survive under new metadata. Rendering and
load/reset require owner-provided exclusion (the existing activity render lock).
The loader itself does not create tasks, dispatch network work, write storage,
or claim a rendered frame. A caller requesting a particular revision cannot
silently accept active-store fallback to an older one; offline startup may.

`ContentPresentation` hosts `BaseTheme::drawContentPage` inside the existing
transfer activity. Presentation is gated against active file writers; queued
and drawing states block new writers until the display call finishes and fonts
are released. Back returns to the transfer view without ending the session.
RSSI updates do not redraw the content page. No new network service is created.

`ContentSessionInput` owns Back/navigation edges on the main task. Back during
a paint latches a view-dismiss request but returns a no-action result; the
activity still services its normal network slice. Once drawing is finished,
that request dismisses the view without another press and cannot fall through
to session exit. A subsequent new Back press in the transfer view still exits
explicitly. Navigation is admitted only for a visible, non-drawing view with no
conflicting writer. The policy has deterministic edge-sequence tests; these
are not physical latency or complete activity-scheduler acceptance.

Status advertises `contentPresentation` when the host callbacks are registered.
`POST /api/pocket/v1/content/present?deviceID=<8upperhex>&revision=<64lowerhex>`
verifies the active revision and queues only its identity/generation. The read-only
`GET /api/pocket/v1/content/presentation` uses the same query and returns the
current receipt without hashing SD or waiting for the render lock. Schema1
receipts contain deviceID, revision, generation and phase (queued/rendered/failed).
Optional additive fields `failure` (none/memory/preparation/display), `heap`
and `block` describe the last deferred cold-admission sample, not current heap.
The two numbers are zero before that sample. Older clients ignore these fields;
new clients still accept receipts without them and treat unknown failures as a
generic failed redraw. Only `rendered` proves driver completion.
Wrong identity or unavailable revision returns409; preparation/admission failure
returns503 without changing stored activation. Identity is not authentication.

After the HTTP callback and client cleanup, the activity services one pending
preparation under RenderLock. It also waits for the upload reply-grace socket
to close. The 16KiB/4KiB cold rendering gates are unchanged, now sampled outside
the request. Enqueue releases the old reloadable card snapshot; the e-ink panel
retains its last frame. Pending metadata costs at most88B in the existing
activity, with no new task, server, framebuffer or separately allocated buffer.
Incidental renders cannot paint before preparation. New HTTP/upload processing
is paused while preparation/paint owns the font budget; existing completed
upload grace is still serviced. No render lock is acquired on ordinary network
slices, so those slices continue sampling GPIO while the display task paints.
Preparation itself still performs synchronous bounded SD work with watchdog
progress callbacks; this change does not prove its physical latency. Deferred rejection is a
revision-bound `failed` receipt, not a lost503; it never retries preparation.
Storage is reverified before drawing, including generation equality. Hide
cancels a pending request; successful display releases fonts as before.
Host tests cover these controller transitions and source-level wiring order;
they do not establish device heap savings or successful physical redraw.

The Swift app separately confirms storage and presentation, sends the paint
request once, and resolves lost responses by reads only. `rendered` means the
existing display-driver call returned, not optical proof. Rendering failure
does not display the partially drawn framebuffer. Controller fault-injection
tests now compile the production ContentPresentation.cpp against fake storage,
font and display boundaries: load/OOM, missing coverage, font-read and paint
failures, delayed driver completion, navigation, hide and explicit recovery.
They verify no display call on failed paint, no implicit retry, and release of
fonts/the writer gate. The controller permits capture only after driver completion.
ActivityManager now checks the activity's capture permission before publishing
a live frame; manual screenshots check the same permission under RenderLock
before writing a BMP or flashing the screenshot border. The transfer activity
also tracks whether its last native/content paint completed, so closing a failed
view cannot expose the damaged buffer before the transfer view repaints. Other
native activities retain their previous capture policy. The last valid live
capture remains historical; a failed content paint emits no new frame.

Controller capture-state transitions are host-tested. Actual manager/screenshot
scheduling and host/device pixel parity remain unverified; the fake theme does
not test actual page pixels or font-facade allocation behavior.

The live path also needs bounded font access: File Transfer unloads SD fonts,
whereas the existing Pocket renderer may rebuild page-sized font caches.
`SdCardFont::BoundedUI` now supplies disk-backed interval/shaping lookups and a
byte-capped glyph cache. The live presentation/font facade selects it, checks
coverage before painting, and releases fonts before admitting another transfer.
Cold admission currently requires16KiB free heap and4KiB largest block; these
are conservative code gates, not measured hardware acceptance thresholds.
See [bounded-ui-fonts.md](bounded-ui-fonts.md) for exact bounds and evidence.

## Live page geometry

`ContentPageRenderer` now owns shared page drawing used by both BaseTheme's
device adapter and the host target. It receives preflighted font ID, theme
spacing/padding, translated empty-state/button strings, immutable card and an
image painter callback. The device adapter retains UITheme/HAL ownership and
post-draw font-I/O checks. The common function neither selects fonts nor performs
storage/network/display calls. Invalid geometry/strings fail before clearing;
an image with available space but no successful callback fails the page.

Real-font host comparison now covers24 text/card/empty frames across two panel
geometries and four orientations. Cached and BoundedUI buffers agree byte for
byte. Cached reference prewarming uses the entire page, not repeated replacement
of its mini kerning matrix per field. This does not prove physical pixels,
composed image output, all native themes or the not-yet-implemented Swift bridge.

The renderer and future host preview share `ContentPageLayout`, an
allocation-free calculation using oriented viewport/bezel dimensions and the
active theme's padding/spacing. Arithmetic widens to int64 before addition or
multiplication and narrows only after the line boxes fit the viewport. Invalid
dimensions, nonpositive line height or oversized layout are refused before
clearing the framebuffer. Negative theme padding/spacing retain the earlier
zero-clamp behavior. Empty content reserves two title lines, the title/body gap,
two body lines and the independent footer; authored question/context line
counts and image height are bounded by the remaining body area.

Host tests cover X3/X4 portrait/landscape dimensions, four rotated asymmetric
insets, exact minimum empty-state height, invalid cursors and int32 extremes.
These prove layout-box arithmetic, not actual glyph ink bearings, real panel
bezel calibration or rendered pixel parity. This shared geometry is not the
still-unimplemented host-renderer ABI or declarative UI-pack runtime.

Content font preflight now uses `checkLayoutText`: validated UTF-8 is checked
in bounded128-byte chunks, with newline/CR/tab normalized to spaces. Those are
layout controls, not glyphs that a font must contain. Wrapping preserves explicit
line breaks and renders internal tabs as spaces. This fixes valid multiline
cards being refused as missing-glyph failures. Real-rasterizer host comparisons
exercise multiline Latin/Korean/Japanese text; complete page/panel parity remains
pending.

On PocketDailyActivity entry, recover the fully verified active revision, then
load its cards with `loadRevisionCards`. Loading reuses full revision verification
and copies the decoded cards during that pass; no unchecked second file read is
used to populate the display snapshot. Any failure clears the entire output.
No record or content file is written by loading.

The immutable snapshot is retained for that activity lifetime and released on
exit under ActivityManager's render lock. At most three cards are shown in
canonical manifest order. They replace the built-in study row when nonempty;
Continue Reading remains first and optional provider cards fill remaining rows.
The existing text detail renderer and personal/retained snapshot use these same
cards. Back/Later closes an app card locally, without creating a provider outbox
entry. Card files cannot supply provider commands or choices.

Allocation is one fallible activity snapshot <=2400B only for a nonempty active
revision, plus the existing temporary verification workspace <1536B and HAL
handles while loading. Text cards require no per-frame SD read/allocation. There
is no new task, framebuffer, network service or permanent global buffer. Missing content falls
back to existing local/provider content; corrupt newest revisions use the active
store's verified older revision when available. An I/O error on the subsequent
load discards the snapshot, rather than exposing partial cards.

## Detail image rendering

The activity retains the selected revision's65-byte identifier with its cards.
For a matching card image, it opens only that published revision's canonical
path through HAL and calls the shared BaseTheme image renderer. PBM validation
precedes pixel output. Rasterization reads <=64B per row, uses integer
nearest-neighbor fitting without upscaling, and writes through the existing
oriented GfxRenderer. The image occupies the remaining area below card text and
above action hints; when there is no space it is omitted. No decoded image or
second framebuffer is allocated. The temporary HAL handle is released after
each draw rather than retaining SD handles while the activity is idle.

Images are reread on detail paints; there is no image cache. Late read failure
clears the reserved image area so a partial raster is not displayed. Source
directories must remain immutable: activity entry verifies manifest/file hashes;
painting revalidates PBM structure, not SHA. Arbitrary external SD edits after
entry are outside this ownership guarantee. The retained/overview card still
shows text; images are currently a detail-view feature only.

## Deliberate limits

- PBM drawing is covered by pure raster tests and compile/source checks of the
  GUI binding, not an actual-panel or host/device pixel-parity test.
- Offline content loads on activity entry. Live content loads only on explicit
  presentation; activation alone does not switch views or prove a repaint.
- A successful content state receipt still means storage selection, not pixels.
- Empty active content falls back to the built-in study experience.
- Host tests exercise real loading with fake storage, including failure after
  card decoding; screen appearance, runtime heap and physical SD behavior still
  require hardware acceptance. No deployment is implied by a local build.
# Navigation hint correction — 2026-09-23

ContentPresentation passes distinct localized previous/next labels through the
existing MappedInputManager mapping. The former pair of identical Prev/Next
labels described neither button's action clearly. Both labels are checked for
font coverage before drawing; English uses Prev/Next as separate labels and
Korean uses 이전/다음. Other languages retain the English fallback until translated.
Input handling, orientation/remapping and the shared renderer are unchanged.
The companion's explicitly English reference preview uses the same separate
labels. This does not establish connected-settings or physical-panel parity.
