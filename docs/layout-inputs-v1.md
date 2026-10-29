# Connected layout inputs v1/v2

Working-tree implementation; hardware acceptance belongs to the parent app run.

`/api/status` advertises `layoutInputs: 2` only when the read-only capture callback
exists on a Sync profile. Clients explicitly request
`GET /api/pocket/v1/layout-inputs?deviceID=<8 uppercase hex>` once when opening or
refreshing a connected layout preview. No polling. Identity, transfer-busy,
render-busy and heap admission apply. No paint, activation, settings write,
font load or framebuffer copy occurs. `Cache-Control: no-store`.

Success is `application/octet-stream`, at most 8192 bytes. PDLI header is
`50 44 4c 49 01 00 00 00` (magic, little-endian u16 version 1, u16 reserved 0).
The field sequence is `preview/LayoutInputs.cpp::fields`: explicit-width
little-endian integers, canonical u8 booleans, strings prefixed by a u16 UTF-8
byte count excluding NUL. Tail is the little-endian CRC32 of every preceding
byte. Unknown version, nonzero reserved bytes, trailing bytes, checksum,
malformed UTF-8, controls, bounds and inconsistent identity/revision/count fail.
The Swift app treats the payload as opaque; the shared native decoder is the
source of truth. No C++ struct bytes or pointers are serialized.

## Schema 2: ordinary Home and powered-off Brief

The current device encodes PDLI version 2 (`50 44 4c 49 02 00 00 00`), still
bounded to8192 bytes. The decoder retains version1 support. After the original
20 strings, v2 adds localized asleep and wake-tab labels, then three u32 values:
`coverKey`, `coverSize`, `coverCrc`. The key identifies the currently selected
open-book cached cover; size is0 for the placeholder or1..131072 bytes. CRC32
binds the immutable BMP bytes. No file path or framebuffer is serialized.
An old app that only accepts capability1 falls back to its labelled sample.

`GET /api/pocket/v1/layout-cover?deviceID=...&key=...&size=...&offset=...`
returns exactly min(4096, size-offset) bytes from that cover. The reader resolves
the current book itself through `HomeCover`, the same path/thumbnail fallback
used by ordinary Pocket Daily Home. Clients cannot select arbitrary SD paths.
Identity, transfer/render exclusion, matching key/size and offset bounds apply;
the app checks the assembled CRC and publishes no exact preview after a change
or short read. The existing4KiB staging buffer is reused. Metadata CRC uses
64-byte reads; no cover-sized allocation or persistent scene exists on firmware.

Additive ABI1 calls `pdui_get_layout_asset_info` and
`pdui_render_reader_device` expose the schema/asset descriptor and paint ordinary
Home or powered-off Brief. Borrowed, validated cover bytes are decoded through
the same Bitmap/GfxRenderer code as the device; no OS file is opened. Sleep uses
the real localized asleep status, requested cover choice, wake-tab inset and
power-switch painter. Existing `pdui_render_reader_home/brief` keep their Sync
placeholder/footer behavior for earlier acceptance and older firmware.

This is point-in-time rendering from reader inputs, not streaming its screen.
Actual-frame acceptance uses developer-only inputs captured at the same paint;
a later ordinary Refresh can observe a newer clock, battery or book state.

## Frozen additive C ABI (ABI version remains 1)

```c
typedef struct {
    char device_id[9];
    char content_revision[65];
    uint32_t physical_width, physical_height, card_count, sleep_cover;
    uint32_t profile_generation, content_generation, font_point_size;
    char font_family[32];
} pdui_layout_inputs_info;

int32_t pdui_get_layout_inputs_info(const uint8_t *snapshot, size_t snapshot_size,
                                   pdui_layout_inputs_info *output);
int32_t pdui_render_reader_home(pdui_context *context, const pdui_profile *profile,
                               const uint8_t *snapshot, size_t snapshot_size,
                               const char *cards_revision);
int32_t pdui_render_reader_brief(pdui_context *context, const pdui_profile *profile,
                                const uint8_t *snapshot, size_t snapshot_size,
                                const char *cards_revision, uint32_t options);
```

All pointers are borrowed for the synchronous call; no snapshot is retained.
`get_layout_inputs_info` writes output only after complete validation, requires
no context/font, and cannot expose partial metadata. Render attempts invalidate
the previous frame even on failure. Context orientation must be portrait and
physical dimensions must match the snapshot. Exact rendering uses BoundedUI
with no glyph fallback, regardless of prior sample-mode context settings.

Use existing `pdui_set_cards` with the verified cards and image bytes pulled
from the snapshot's `content_revision`. Pass that exact revision to rendering;
revision and count mismatches reject. Empty revision means no active revision
and zero cards. An active empty collection still has a nonempty revision.
Recheck device identity and active revision/generation around content pull.
Never supply an unsent local editor draft under the reader revision.

`options` accepts only `PDUI_SLEEP_BOOK_COVER` (0 or 2); proposed cover changes
may render locally. WAKE is rejected. Snapshot `sleep_cover` is the current
reader value, while the passed profile and options are proposed values.
`font_family` is `PocketSansWorld`; `font_point_size` is the registry's resolved
nearest-12 size, zero if unavailable. Clients need matching font bytes and must
label unsupported fonts as sample/unavailable instead of claiming exactness.

## Rendering scope and input ownership

The snapshot contains the open book's bounded title/author and stored whole-book
percent, the actual rolled glance with saved metadata and staleness, the actual
daily word even when the current profile hides it, resolved UI labels, current
Home geometry and full passive header settings (theme alignment/underline,
battery size/percentage/charging/label, clock text/placement, status strip and
button geometry). The proposed profile is absent. No private book path, image,
credential or document body is included.

Both device BaseTheme and native host call `components/themes/HeaderPainter.cpp`, which
uses the same FreeInkUI header/battery components and built-in UI fonts. Home
and Brief use the production HomeRenderer and card summary helpers. Actual
Sync book covers are placeholders; the preview deliberately uses those same
placeholders. Brief says the resolved sleep-preview label and has no WAKE cue.
This is the canonical Sync Apply frame, not a mirror of decoded book covers,
an arbitrary current physical screen or deep sleep. Snapshot time is fixed.
When a successful Sync presentation for the current profile generation still
owns the display, the endpoint uses that presentation's exact header inputs.
The e-paper header may still show 91% after the sensor reaches 92%; returning
the newer sensor value would incorrectly change the companion preview. The
controller retains only a request-scoped header receipt (at most 192 bytes),
released on replacement, hide or failure. It publishes the receipt only after
the display call completes, under the existing RenderLock. No framebuffer,
card data or full scene is retained. Without a matching presentation, current
header inputs describe the next proposed layout. A fresh Apply updates the
physical header; Refresh reads the matching presented values and current
content inputs. Offline/demo/old firmware remain labelled samples.

## Implementation and verification — 2026-10-18

The generic shared header lives in `src/components/themes/HeaderPainter.*`;
stock themes include no Pocket product code. `BaseTheme` keeps its original
`uiScaleSpec`, theme-token refresh and touch-back target. A capture resolves the
same title/subtitle styles, popup normal style and actual font bindings without
publishing a theme change. Bitmap-backed header paints are not supported by
schema 1 and are rejected instead of silently approximated.

Cold request allocations are fallible and request scoped. Native arm64 sizes
are 5,832 bytes for LayoutInputs, 184 for HeaderInputs and 2,728 for SDK theme
tokens (device pointer-width sizes may be smaller). Capture overlaps the
existing frame/cards and a temporary exact-theme description; the frame is
released before encoding. The response buffer is allocated at its measured
encoded size and the input object is freed before the socket write. Admission
requires 24 KiB free heap and an 8 KiB largest block, plus existing transfer and
render exclusion; this is not measured proof of a safe minimum on every board.
There is no persistent snapshot cache or second framebuffer. Large aggregate
clears use in-place construction, avoiding a second stack-sized temporary.

Font binding changes occur inside the normal exception boundary, never in an
allocating destructor. Exact calls select BoundedUI; every sample/content call
selects its own original mode. A partial registration failure clears the bound
marker so the next attempt rebuilds both maps. No allocator-failure test hook
exists in this host API; normal alternating cached/exact rendering is covered.

Focused native evidence in ignored `build/`:

- `layout-inputs-results.xml`: 10/10 tests, including every truncation and
  single-byte corruption, checksummed malformed fields, metadata nonpublication
  on failure, frame invalidation, geometry/revision/count/font/WAKE rejection,
  actual book/empty/word, nonempty weather/events/staleness, header value changes,
  and repeated sample/exact mode switching.
- `LayoutInputs.SharedHeaderMatchesOriginalThemePathAcrossVariants` compares
  the shared painter to an independent transcription of the original passive
  BaseTheme path across eight variants, including title alignment, underline,
  battery side/charging/hidden percent, clock placement, short bands, back icon
  and Korean title bytes. It protects existing renderer output; it does not
  claim that a built-in font gains missing Korean glyph coverage.
- `layout-inputs-host-regressions.log`: all 89 graphics-host tests pass.
- `layout-inputs-routes.log`: all 20 existing route/boundary tests pass.
- `layout-inputs-fault-control/result.json`: an isolated compiled codec with
  its CRC check removed fails the corruption regression as expected. Production
  source and the imported artifact were not modified for this fault injection.
- `layout-inputs-golden.pdli`: 698-byte synthetic fixture, identity A1B2C3D4,
  profile generation 7, no active content, X4 physical 800×480, PocketSansWorld
  12. Regenerate with `PDUI_LAYOUT_FIXTURE=<path>` and the focused test
  `LayoutInputs.GoldenMetadataAndCanonicalRoundTrip`. It contains no reader data.

`host/build_apple.py` now pins the generic header, its icon and FreeInkUI
sources/headers as compiler dependencies. The dependency validator passed for
all 341 recorded source files in the focused host build; no validation was
relaxed. Parent app/hardware verification remains separate from these native
checks. A later device/app acceptance report must identify the exact installed
firmware and host artifact before claiming physical full-frame parity.
