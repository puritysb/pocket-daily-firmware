# Production rasterizer on the host

This target links the independent `host/` library with the actual GfxRenderer, bitmap/dither helpers, EpdFont,
SdCardFont, FontCacheManager, FontDecompressor, InflateReader and MiniBidi sources.
The memory display replaces only HAL. Software text/pixel drawing is not mocked.
Hardware image blits and grayscale operations throw explicitly; they do not
claim successful output. Physical panel timing/waveforms are not simulated.

The font loader uses `host/hal/HalStorage.h`, a read-only borrowed-asset adapter.
Callers retain immutable names/bytes and bind an AssetScope around every font
load/render operation. Bindings are thread-local and nested scopes restore their
predecessor; open handles keep independent cursors and cannot be redirected by
a nested scope. Unknown/duplicate names fail closed; no filesystem path is opened.
The adapter has no write API and adds no device allocation. Context owners must
serialize use of their own font/renderer and keep borrowed bytes alive through
all reads; thread-local binding is not a lock on shared renderer objects.
The C ABI now owns font copies and binds this scope internally. It serializes
rendering across contexts because MiniBidi shares static scratch. This is not a
complete theme renderer, app bridge or device/host golden acceptance. Nothing is copied
into the companion app. Host-only allocations/exceptions do not ship on ESP32.
The display buffer has16-byte canaries on each side and a2048x2048 dimension
bound; the font fixture input is bounded to64MiB. These are host limits only.

Build and run ordinary tests through the repository host CMake target. To compare
the local bundled font using the actual rasterizer:

```sh
build/host-tests/gfx_host/gfx_font_fixture_check \
  firmware/sd-card/.fonts/PocketSansWorld/PocketSansWorld_12.cpfont
```

The comparison covers two physical buffer geometries, four orientations,
regular/bold Latin/Korean/Japanese text, normalized whitespace, shaping and
shared wrapping. It also calls the same ContentPageRenderer used by the device
theme for a text card, empty content and image card:32 frames total. Cached reference mode
prewarms all page strings together because each prewarm replaces its mini kerning
matrix; BoundedUI has no page cache. It requires nonblank, byte-identical Cached/BoundedUI frames
and intact buffer canaries. This compares two modes on the **same host**, not
physical device output. The font fixture is a local artifact, not a network
download or connected reader prerequisite for normal CTest.

ContentPageRenderer is shared production drawing, not a separately implemented
preview. The device BaseTheme adapter supplies current metrics/translations,
preflighted font and HAL-backed image callback. Host unit tests use a synthetic
font to check missing image callbacks, callback failure, invalid options and
absence of implicit display calls. ContentImageRenderer also shares the device's
image rasterization, clipping and failure clearing without another framebuffer.
An independent bit-pattern oracle checks image pixels on both geometries in all
four orientations. Late read failures must clear partial image output, preserve
outside pixels and never present a frame. The local real-font comparison covers
composed image cards. The content C ABI also byte-matches24 page frames against
direct calls and checks invalidation/recovery. Synthetic-font CTest exercises
the public API without external assets, and a C-only consumer verifies linkage.
Other native theme surfaces and app integration remain pending; see
`../../host/README.md` for the independent build and actual Swift interop check.

The vendored inflater omits checksum implementations from the firmware's unused
checked-stream path. Host linking supplies zlib-backed checksum adapters with
tested seed/finalization conventions; the decompressor itself remains upstream
production code. Existing production-source unused-parameter warnings may appear
in this broader host build and are not suppressed globally.
