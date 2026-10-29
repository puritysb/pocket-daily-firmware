# Pocket Daily Firmware

Pocket Daily is offline-first firmware for Xteink X3/X4: resume the book already
on the device, carry a small daily study deck, retain last-synced weather and
schedule context, and keep a useful E-ink sleep frame without requiring Wi-Fi.

The firmware does not depend on any daemon or cloud service. Weather and
today's events come from the companion app over Pocket Sync
([`docs/pocket-glance-v1.md`](./docs/pocket-glance-v1.md)); the former
AgentDeck daemon integration was removed on 2026-09-25.

Pocket Daily is built on the open-source [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader)
engine and preserves its reader, storage, format, and recovery foundations.

The independent [Pocket Daily app](https://github.com/puritysb/pocket-daily)
provides the account-free iOS and macOS management experience for books,
learning packs, firmware, diagnostics, and direct SD-card transfers.

## Pocket Daily additions

- Offline-first Pocket Home with local continue-reading and daily study cards.
- SD-backed Japanese learning packs: offline daily kanji with licensed,
  integrity-checked content and optional updates through Pocket Sync.
- SD-backed weather and today's schedule sent by the companion app.
- Retained sleep screen based only on locally durable information.
- Coverage-tested `PocketSansJP` UI font bundle with reproducible OFL sources.

## CrossPoint foundation

[![Fund contributors](https://img.shields.io/badge/%F0%9F%91%91_Fund_contributors-royalty.dev-BB953A?style=for-the-badge&labelColor=1a1a1a)](https://app.royalty.dev/crosspoint-reader/crosspoint-reader)

CrossPoint is open-source e-reader firmware - community-built, fully hackable, free forever. It's maintained by a growing community of developers and readers who believe your device should do what you want - not what a manufacturer decided for you.

**Now running on:** ESP32C3-based Xteink [X4](https://www.xteink.com/products/xteink-x4) and [X3](https://www.xteink.com/products/xteink-x3).

![CrossPoint Reader running on Xteink device](./docs/images/cover.jpg)

## What can CrossPoint do?

- **Reader engine**: EPUB 2/3 rendering with embedded-style option, image handling, hyphenation, kerning, chapter navigation, footnotes, bookmarks, go-to-percent, auto page turn, orientation control, focus reading, KOReader progress sync and more. 

- **Various formats**: native handling for `.epub`, `.xtc/.xtch`, `.txt`, and `.bmp`.

- **Screenshots.**

- **Custom fonts**: install your favorite fonts on the SD card.

- **Tilt page turn (X3 only)**.

- **Library workflow**: folder browser, hidden-file toggle, long-press delete, recent books, SD-cache management.

- **Wireless workflows**:
  
  - File transfer web UI
  - EPUB Optimizer
  - Web settings UI/API (edit many device settings from browser)
  - WebSocket fast uploads
  - WebDAV handler
  - AP mode (hotspot) and STA mode (join existing Wi-Fi), both with QR helpers
  - Calibre wireless connect flow
  - OPDS browser with saved servers (up to 8), search, pagination, and direct download
  - OTA update checks and installs from GitHub releases

- **Customization**: multiple themes (Classic, Lyra, Lyra Extended, RoundedRaff), sleep screen modes, front/side button remapping, status bar controls, power-button behavior, refresh cadence, and more.

- **Localization**: 24 UI languages and counting. RTL support.

### Coming soon:

- Dictionary lookup — inline word lookup without leaving the reader.

- More themes.

- Much more! stay tuned.

---

## Model compatibility and USB-locked readers

Identify the exact model, installed firmware and installation eligibility
before choosing an image. This product targets X3 and X4 with ESP32-C3; X4
Classic/V2 and X4 Pro are different devices. An unfamiliar model or USB lock
is not a reason to try a generic X3/X4 image.

Do not use the CrossPoint unlocker to install Pocket Daily. The
[upstream installation guidance](https://github.com/crosspoint-reader/crosspoint-reader#install-firmware)
limits that unlocker to CrossPoint and CrossInk; installing CrossPoint first
does not establish that a locked unit can safely run or recover from this fork.
For locked or uncertain units, consult the seller or model-specific upstream
instructions and use the app's offline reading meanwhile. Factory and
USB-locked starting states have not been verified on hardware for Pocket Daily.

## Install firmware

Pocket Daily releases are published at
[github.com/puritysb/pocket-daily-firmware/releases](https://github.com/puritysb/pocket-daily-firmware/releases).
One `firmware.bin` serves the supported ESP32-C3 X3 and X4 models.

As checked on 2026-10-25, the current product release `pocket-v0.1.0` has not
been published. GitHub's latest release is the historical `v1.6.6`; the current
app excludes it and the retired pre-reset betas. The update instructions below
require a supported `pocket-v…` release. Local-image selection belongs to the
Debug-only development path below; a local build is not a published release.

### Update a reader that already runs Pocket Daily

- **From the Pocket Daily app (recommended):** on the reader press **Sync** →
  **Same Wi-Fi**, then tap **Update reader** in the app. The app downloads the
  latest release, validates it and sends it. When you leave the transfer
  screen, the reader shows **Update firmware?** and flashes only after you
  confirm. For Direct connection, prepare the official update before joining
  the reader's private network. The shipping app does not import local firmware
  files; **Send a Local Build…** is available only in Debug builds.
- **On X4:** press **Sync** and choose **Check for updates** (also under
  Settings). The reader joins a saved Wi-Fi network and installs only after you
  confirm. This path still needs acceptance on the exact release image.
- **On X3:** use the app or SD-card update. Reader-native HTTPS update checks
  and downloads are outside the current 0.x support scope because the stock
  TLS buffers exceed available memory. The reader explains the memory failure
  and points to the app; this is not a successful on-reader update check.

Nothing installs automatically after a transfer. After confirming installation
on the reader, reconnect to the same reader and check its reported version.
Keep a previous compatible image available for SD-card recovery. The model-specific
acceptance criteria are in [the release checklist](docs/release-checklist.md).

### Move from CrossPoint Reader

On an eligible reader, open File Transfer → Join a Network. In the app choose
Find on Same Wi-Fi, then Manage Reader → Firmware → **Install Pocket Daily…**.
The shipping app needs a supported official release; it validates the image,
copies `pocket-daily.bin` to the SD root and reads the file back for SHA-256
verification. On the reader press Back → Settings → System → SD Card Firmware
Update, choose `pocket-daily.bin` and confirm.

This route passed on one X4 with a CrossPoint `1.4.1-dev` fork on 2026-10-16.
On the same X4 with upstream CrossPoint `1.6.5`, upload/readback failed under
low heap; a clean-card comparison remains untested. Copying a published image
to SD with a card reader is the fallback, but that complete route has not yet
been rehearsed. The later developer USB installation does not establish that
the failed Wi-Fi route is fixed. The companion repository's `docs/OOBE.md`
records these starting states and their route-specific evidence.

### First install from factory firmware

The app cannot connect directly to factory firmware or bootstrap it wirelessly.
Confirm the exact model, software and installation eligibility with the seller
or manufacturer instructions; app-only reading works while this is unresolved.
Factory-firmware installation has not been verified on hardware.

A customer-facing Pocket Daily USB installer, in the app or a web flasher, is
not built. USB serial installation is a developer procedure today, and a
shipping-app flashing path needs a separate product decision. This README does
not establish a customer USB installation route for factory or locked readers.

### Revert to CrossPoint or official firmware

Confirm a compatible recovery image and the exact model's supported recovery
method before installation. Do not assume a locked unit can return to other
firmware. The release checklist requires SD recovery acceptance on both target
models; earlier developer recovery evidence is not approval of every starting
state.

### Development builds

Every successful local build stages `firmware/update.bin`. Send it with the
Debug app's **Send a Local Build…** (or **Copy a Local Build…** for CrossPoint),
use `scripts/pocket_put.py`, or copy it to the SD card root and install from
Settings. Local firmware selection is absent from the shipping app, and a
Debug transfer does not sign off its official-release update flow. Authorized
developer hardware procedures are in
[the developer pipeline](docs/developer-pipeline.md); see also
[Development quick start](#development-quick-start).

### Releasing

Set `version` in `platformio.ini` and complete
[the release checklist](docs/release-checklist.md) for the exact production
commit and image on X3 and X4. Only after owner authorization, publish a
`pocket-v<version>` tag (for example `pocket-v0.1.0`). The workflow builds
`gh_release`, verifies its embedded version and publishes a normal GitHub
release marked latest. The old `vX.Y.Z` tags do not trigger this workflow.

`pocket-v<version>-beta.<n>` builds `gh_release_beta` and publishes a prerelease
for development apps; it does not replace latest or establish stable hardware
acceptance. See [product versioning](docs/product-versioning.md) for the 0.x
support scope and the full `1.0.0` gates.

---

## Custom SD-card fonts

Convert your own TTF/OTF files into `.cpfont` files that load from the SD card. No firmware reflash is needed.

1. Go to https://crosspointreader.com/fonts and open the "SD-card font builder" form.
2. Upload up to four styles (regular, bold, italic, bold-italic), set the family name, point sizes, and Unicode range.
3. Download the generated `.cpfont` files.
4. Copy them to your SD card under `/fonts/YourFont/` (or `/.fonts/YourFont/` to hide the folder).
5. Select the font on the device from the font settings.

Conversion runs the firmware repo's `lib/EpdFont/scripts/fontconvert_sdcard.py` script unmodified, so output matches a local host build.

---

## Documentation

- [User Guide](./USER_GUIDE.md)
- [Web server usage](./docs/webserver.md)
- [Web server endpoints](./docs/webserver-endpoints.md)
- [Bilingual book pipeline](./docs/bilingual-pipeline.md)
- [Project scope](./SCOPE.md)
- [Contributing docs](./docs/contributing/README.md)

---

## Development quick start

### Prerequisites

- [pioarduino](https://github.com/pioarduino/pioarduino) or VS Code + pioarduino plugin
- Python 3.8+
- `clang-format` 21
- USB-C cable supporting data transfer

### Setup

```bash
git clone --recursive https://github.com/puritysb/pocket-daily-firmware
cd pocket-daily-firmware

# keep the reader-engine source available for controlled merges
git remote add upstream https://github.com/crosspoint-reader/crosspoint-reader.git

# if cloned without --recursive:
git submodule update --init --recursive
```

### Build / flash / monitor

```bash
./scripts/pio.sh run --target upload
```

### Contributor pre-PR checks

```bash
./bin/clang-format-fix
./scripts/pio.sh check -e default
./scripts/pio.sh run -e default
cmake -S test -B build/host-tests -DCMAKE_BUILD_TYPE=Release
cmake --build build/host-tests
ctest --test-dir build/host-tests --output-on-failure -j
```

### Debugging

After flashing the new features, it’s recommended to capture detailed logs from the serial port.

First, make sure all required Python packages are installed:

```python
python3 -m pip install pyserial colorama matplotlib
```

After that run the script:

```sh
# For Linux
# This was tested on Debian and should work on most Linux systems.
python3 scripts/debugging_monitor.py

# For macOS
python3 scripts/debugging_monitor.py /dev/cu.usbmodem2101
```

Minor adjustments may be required for Windows.

---

## Internals

CrossPoint Reader is pretty aggressive about caching data down to the SD card to minimise RAM usage. The ESP32-C3 only has ~380KB of usable RAM, so we have to be careful. A lot of the decisions made in the design of the firmware were based on this constraint.

### Data caching

The first time chapters of a book are loaded, they are cached to the SD card. Subsequent loads are served from the
cache. This cache directory exists at `.crosspoint` on the SD card. The structure is as follows:

```text
.crosspoint/
├── epub_<hash>/         # one directory per book, named by content hash
│   ├── progress.bin     # reading position (chapter, page, etc.)
│   ├── cover.bmp        # generated cover image
│   ├── book.bin         # metadata: title, author, spine, TOC
│   ├── css_rules.cache  # parsed CSS rule cache
│   ├── img_*            # rendered image cache files
│   └── sections/        # per-chapter layout cache
│       ├── 0.bin
│       ├── 1.bin
│       └── ...
├── settings.json        # device settings
├── state.json           # resume/runtime state
└── recent.json          # recent books list
```

Removing `/.crosspoint` clears all cached metadata and forces a full regeneration on next open. Book deletes, overwrites, and moves done through the firmware or web UI clear or re-key matching caches; manual SD-card edits may leave stale cache directories behind.

For more details on the internal file structures, see the [file formats document](./docs/file-formats.md).

---

## Contributing

Contributions are welcome. If you're new to the codebase, start with the [contributing docs](./docs/contributing/README.md). For things to work on, check the [ideas discussion board](https://github.com/crosspoint-reader/crosspoint-reader/discussions/categories/ideas) — leave a comment before starting so we don't duplicate effort.

Everyone here is a volunteer, so please be respectful and patient. For governance and community expectations, see [GOVERNANCE.md](./GOVERNANCE.md).

---

## Community forks

One of the best things about open source is that anyone can take the code in a different direction. If you need something outside CrossPoint's [scope](./SCOPE.md), check out the community forks:

- [CrossInk](https://github.com/uxjulia/CrossInk) — Typography and reading tracking: Bionic Reading (bolds word stems to create fixation points), guide dots between words, improved paragraph indents, and replaces the default fonts with ChareInk/Lexend/Bitter.

- [papyrix-reader](https://github.com/bigbag/papyrix-reader) — Adds FB2 and MD format support. Actively maintained with Arabic script support. Custom themes via SD card.

- [crosspet](https://github.com/trilwu/crosspet) — A Vietnamese fork that adds a Tamagotchi-style virtual chicken that grows based on your reading milestones (pages read, streaks, care). Also: Flashcards, Weather, Pomodoro timer, and mini-games.

- [crosspoint-reader-cjk](https://github.com/aBER0724/crosspoint-reader-cjk) — Purpose-built for Chinese, Japanese, and Korean reading.

- [inx](https://github.com/obijuankenobiii/inx) — Completely reimagines the user interface with tabbed navigation.

- ~~[PlusPoint](https://github.com/ngxson/pluspoint-reader) — custom JS apps support.~~ (Unmaintained)

- [crosspoint-reader-papers3](https://github.com/juicecultus/crosspoint-reader-papers3) — Crosspoint port for M5Stack Paper S3. 

- [t5s3-reader](https://github.com/ShallowGreen123/t5s3-reader) — Crosspoint port for LilyGo T5 ePaper S3 / T5S3 4.7-inch e-paper device.

**Note:** Many of these features will make their way into CrossPoint over time. We maintain a slower pace to ensure rock-solid stability and squash bugs before they reach your device.

Want to build your own device? Be sure to check out the [de-link](https://github.com/iandchasse/de-link) project.

---

CrossPoint Reader is **not affiliated with Xteink or any device manufacturer**.

Huge shoutout to [diy-esp32-epub-reader](https://github.com/atomic14/diy-esp32-epub-reader), which inspired this project.
