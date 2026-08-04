---
name: firmware-deploy
description: Getting a built firmware image onto the device. Use when flashing, uploading, installing, or deploying firmware, when iterating without a USB cable, when asked how to test a build on hardware, or when setting up an SD-card update flow. Covers pio upload targets, the auto-staged firmware/ directory, recovery-mode SD install, and auto-copy to a mounted card reader.
---

# Firmware Deploy

Two paths onto the device: USB (`pio run -t upload`) and SD card. The SD path exists because
USB iteration is slow and the cable is often not where the device is — prefer it once the
build is stable.

## USB

```bash
pio run -t upload                        # build + flash over USB
pio run -t upload && pio device monitor  # flash, then watch serial
pio run -t uploadfs                      # filesystem image only (SPIFFS/LittleFS)
```

## SD card (no USB)

Every successful `pio run` auto-stages the binary via `scripts/stage_firmware.py`:

```
firmware/                                      # gitignored
  update.bin                                   # always latest — copy this to SD card
  crosspoint-<version>-<env>-<sha>-<date>.bin  # versioned archive (keeps last 5)
  LATEST_BUILD.txt                             # version, commit, size, install steps
```

**Install:**
1. Copy `firmware/update.bin` to the root of the SD card.
2. Hold **UP + POWER** to boot into recovery firmware mode.
3. Pick `update.bin` from the file browser and confirm.

`LATEST_BUILD.txt` is the authority on what a given `update.bin` actually contains — read it
before claiming a build is on the device.

**Auto-copy to a mounted card reader** — set `FIRMWARE_STAGING_DIR` (env var) or add
`firmware_staging_dir` to `platformio.local.ini` (never `platformio.ini` — the path is
machine-specific):

```ini
# platformio.local.ini
[env:default]
firmware_staging_dir = /Volumes/SDCARD
```

## Before handing off

- Did the build actually succeed? A stale `update.bin` from a previous run looks identical.
- Building a release image? Use the right env (`gh_release`, `gh_release_rc`) — the default
  env ships serial logging at `LOG_LEVEL=2`.
- Flashing is a hardware action with a failure mode you cannot see from here. State plainly
  which artifact you staged and let the user do the install and confirm.
