# CrossPoint 2.0.0 baseline integration

Prepared on 2026-11-06 on `feature/upstream-2.0.0`. Owner decision
(2026-11-06): ship Pocket Daily 1.0.0 on the CrossPoint 2.0.0 base even if that
delays the release, and use the 2.0 plugin system to make Pocket Daily easy to
reach for stock CrossPoint users. Nothing in this integration was installed or
tested on a physical reader.

## Pinned inputs

- Product parent: `7c4e9fb1466810734c31053494d14276dff4f166`.
- Previous upstream baseline: CrossPoint 1.6.5, `93e98bb7`.
- Integrated upstream tag 2.0.0: `42d58daaabcf469a2e5e1ba871714fb9dcf51d29`.
- FreeInk SDK: `5e94fe7480f019c89fbf691aef87523eb1b46704` (was `111fdcc7`).

The product version remains **1.0.0** (`docs/product-versioning.md`). The
CrossPoint baseline number is not the product number.

## Strategy: fork reader engine, upstream shell and services

2.0.0 rewrote large parts of the engine-adjacent code that the fork had already
diverged in (bounded fonts, raw DOM offsets, layout ahead, incremental
pagination). Re-porting those features onto 2.0's engine would risk every
companion reading-sync contract. The integration therefore keeps the fork's
engine and settings, and adopts upstream where the change is a self-contained
service.

| Boundary | Resolution |
| --- | --- |
| Reader engine | Fork-owned: `lib/EpdFont`, `lib/GfxRenderer`, `lib/Epub`, `lib/Txt`, `lib/InflateReader`, `lib/uzlib`, `lib/MiniBidi`, `src/activities/reader/*`, KOReader `ProgressMapper`/`ChapterXPathResolver`, dictionary utilities. Companion raw-offset contract unchanged. |
| Settings / web server | Fork-owned `CrossPointSettings`, `SettingsList.h`, `SettingsActivity`, `CrossPointWebServer`, `WebDAVHandler`, fork HTML pages. Upstream's settings rewrite and new web assets are not taken. |
| Plugins | Adopted (catalog, manifest classification, picker, events, SD plugins). **Default off**: new `pluginsEnabled` setting (System). A migration enables it when a plugin is already installed on the card, so a stock-CrossPoint user who flashes Pocket Daily keeps their plugins. |
| Plugin HTTP | `PluginHttp` runs over the fork's certificate-verified `HttpDownloader::request` (GET/POST/PUT/PATCH/DELETE/HEAD, no redirects, 24 response headers max, 512 B stack chunk). `SecureHttpClient`/wolfSSL are not linked. |
| Plugin delivery on sleep | Only over an already-connected STA link (`WiFi.status() == WL_CONNECTED`); the reader never joins Wi-Fi just to deliver a plugin event. |
| OPDS 2.0 | Adopted via the SDK `Opds`/`JsonSax`/`CatalogList` libs over `OpdsHttpTransport`. Credentials are bound to the configured server origin (`HttpOrigin`): feeds, covers and downloads never send them to another origin. |
| TLS | One engine: ESP-IDF `esp_http_client` + `esp_crt_bundle`. `FREEINK_CONTENT_WOLFSSL` stays undefined. |
| LCP | Compiled out (needs wolfSSL); LCP books report `STR_LCP_UNSUPPORTED`. |
| BLE page turner | `lib/BlePageTurner` adopted with its `NoRadio` port only; the radio stays off. Product BLE remains the Pocket Daily path. |
| Device fixes | Adopted: HAL/power fixes, KOReader custom headers, LibraryBuilder XTC metadata, hidden-item rule (`pocket-daily` added). |
| Deferred | Upstream browser-plugin web routes (relay, crypto, fetch, plugin host), reader plugin events from the fork reader, clippings, GIF decoding, TXT-via-EPUB, upstream engine features (selection, link-note titles, chapter build gate), the upstream settings list UI. Each needs a separate RAM/flash case. |

## Flash budget and the bold-italic decision

A first combined build was 6,745,987 B against the 6,553,600 B OTA slot.
Measured against a 1.6.5-base build of the same tree (6,490,765 B): plugins
about +60 KB, OPDS 2.0 about +30 KB, i18n +72 KB (used keys 602 → 674; the
generator already strips unused keys).

Resolution: drop the built-in bold-italic faces for all eight font families;
the renderer falls back to the bold face for bold-italic runs. No other
reading feature was removed. Because cached layouts measured the old faces,
`SECTION_FILE_VERSION` moved 137 → **138** (bytes unchanged; next free fork
number 139, `lib/Epub/AGENTS.md`). **Product-visible: bold-italic text now
renders upright bold.** Owner confirmed this trade on 2026-11-06.

| Environment | Image bytes | Remaining OTA slot bytes | SHA-256 |
| --- | ---: | ---: | --- |
| default | 6,238,592 | 314,976 | `dc1ff7150d5f4474a6eaf1b1e6fbe922eb2411f32309f3cc29f293cd57dcb6e5` |
| gh_release | 6,119,696 | 433,904 | `e669eccd84e6fa88a27f56ae251fbcd9bcd900a99a9a7b292e8392d5cb6bbb3e` |

Images were built from the uncommitted tree on parent `7c4e9fb1`; identify them
by hash, not version string. Rebuild after the commit before any device test.

Static RAM (`.data` + `.bss`): default 73,296 B versus 71,544 B on the 1.6.5
base (+1,752 B); gh_release 71,752 B. Static size does not measure runtime heap
or the largest free block.

## Status and update compatibility

- `/api/status` mirrors the new 2.0 base fields `firmwareFamily` and
  `pluginApi`; `crossPointBase` reports `2.0.0`. Not mirrored: `serial`,
  per-plugin `deviceId`, `pluginCapabilities` (`docs/SEAM.md`).
- Stock 2.0 adds a board tag check to SD and OTA updates: only an image whose
  `CROSSPOINT-BOARD-V1:<board>;` tag names a different board is rejected. Pocket
  Daily images carry `x4`, the same tag as stock's shared X3/X4 image, so stock
  2.0's SD Card Firmware Update should accept them and Pocket accepts a stock
  image back for rollback (source reading only; not tried on a reader).
- Stock 2.0's Settings "General" section is labelled "System" in English, so
  the app's "Settings → System → SD Card Firmware Update" guidance holds.

## Verification (host only)

- Default and gh_release builds: success, zero compiler warnings.
- Strict cppcheck (low/medium/high): no defects.
- Full host suite: **1,248/1,248** passed. New suites: `url_utils`,
  `opds_feed_error`, `button_press_navigation`, `zip_file`, `http_header`,
  `plugin_catalog_paging`, `plugin_manifest_classification`,
  `plugin_picker_layout`, `protected_paths`, `opds_http_transport`.
  `opds_http_transport` (credential origin) fails under an injected fault.
- `./bin/clang-format-fix` applied.

Build hazard found: two PlatformIO builds of different projects running at the
same time corrupt the shared `~/.platformio` framework libraries ("ROM libc
configuration" error). Build sequentially.

## Not verified — hardware acceptance required

- X3 and X4 runtime heap and largest free block with plugins off and on,
  including Wi-Fi bring-up and HTTPS OTA (X3 OTA was already memory-limited).
- Bold-italic rendering and v138 cache rebuild of existing books, all four
  orientations.
- OPDS over HTTPS with credentials, including a cross-origin redirect.
- Plugin catalog browse/install and a plugin HTTP request on device.
- Companion Bluetooth/Nearby Sync and reading-position round trip.
- Stock CrossPoint 2.0 → Pocket Daily SD update path with plugins installed,
  and the downgrade/rollback path.

## Plugin distribution plan

- **A (this branch):** plugin compatibility in the firmware, default off,
  auto-on when the card already holds plugins.
- **B (companion app, `pocket-daily`):** a "CrossPoint reader" mode that installs
  Pocket Daily firmware through stock CrossPoint's `/upload` plus SD update.
  Not started; needs app-side tests against the stock web server.
- **C (Hub plugin):** `distribution/crosspoint-plugin/` holds the
  `pocket-daily-installer` payload: a stock 2.0 JSON catalog over
  `releases.json` on `main` that downloads a release's `firmware.bin` to
  `/pocket-daily.bin` for the stock SD updater. Validated by
  `scripts/check_crosspoint_plugin.py` in CI. Not published: Plugin Hub
  listing needs a public repository with the `crosspoint-plugin` topic and a
  release, which the owner approves first. Not tried on a reader.
