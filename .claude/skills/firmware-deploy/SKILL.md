---
name: firmware-deploy
description: Getting a built firmware image onto the device. Use when flashing, uploading, installing, or deploying firmware, when iterating without a USB cable, when asked how to test a build on hardware, or when setting up an SD-card update flow. Covers pio upload targets, the auto-staged firmware/ directory, recovery-mode SD install, and auto-copy to a mounted card reader.
---

# Firmware Deploy

Two paths onto the device: USB (`./scripts/pio.sh run -t upload`) and SD card. The SD path exists because
USB iteration is slow and the cable is often not where the device is — prefer it once the
build is stable.

## USB

```bash
./scripts/pio.sh run -t upload                              # build + flash over USB
./scripts/pio.sh run -t upload && ./scripts/pio.sh device monitor  # flash, then monitor
./scripts/pio.sh run -t uploadfs                            # filesystem image only
```

## SD card (no USB)

Every successful `./scripts/pio.sh run` auto-stages the binary via `scripts/stage_firmware.py`:

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

## Over Wi-Fi from the Mac (preferred for iteration)

The reader's own transfer path is the fastest way to get a build onto it and
doubles as a transport test. Card swapping is the fallback, not the default.

1. On the reader: File Transfer → Join a Network (STA on the home Wi-Fi).
   The X3 profile has no mDNS, so find it by probing `/api/status`:
   ```bash
   python3 - <<'EOF'
   import concurrent.futures, urllib.request
   def p(ip):
       try: return ip, urllib.request.urlopen(f"http://{ip}/api/status", timeout=1.2).read(200)
       except Exception: return None
   hosts=[f"192.168.{n}.{i}" for n in (68,69,70,71) for i in range(1,255)]
   with concurrent.futures.ThreadPoolExecutor(256) as ex:
       print([r for r in ex.map(p, hosts) if r])
   EOF
   ```
2. Push and commit the staged image with the repository tool, which prints
   every reader reply verbatim (`RESUME`, `OK`, `ERROR`, commit JSON):
   ```bash
   python3 scripts/pocket_put.py firmware/update.bin --target update.bin --host <reader-ip>
   ```
   `published /update.bin` is staging success, not installation. The tool now
   retries transport failures up to three times with the same staging path and
   verified device ID; `--attempts 1` disables automatic recovery. After exhaustion, rerun with the
   printed `--resume --staging …` options (firmware with `uploadStreamResume`).
   Legacy bootstrap pacing can use `--legacy-delay-ms 20 --legacy-chunk-bytes 512`.
   This changes sender write granularity only; it is not SD acknowledgement and
   does not override the negotiated 4 KiB window on newer firmware.
   For the experimental low-heap STA image, `--flow-delay-ms 10` sends
   512-byte fragments with pacing inside each negotiated 4 KiB window; the
   next window still waits for the reader's SD ACK. Keep status/preview probes
   out of the active payload phase. CLI pacing is opt-in, not a throughput claim.
3. On the reader: Settings → System → Update firmware, then reboot.
4. Verify: `/api/status` `version` must end with the `-w<fingerprint>` of the
   image you pushed (`strings firmware/update.bin | rg 'CrossPoint version'`).

### Explicit developer iteration (not production auto-flashing)

When the user requests automated developer installation, an already-installed
`ENABLE_DEV_REMOTE_FLASH` build supports:

```sh
python3 scripts/pocket_put.py firmware/update.bin --host <reader-ip> --target update.bin \
  --resume --dev-flash
```

The default remains staging only. The new build version is read from the image's
embedded identity strings (including low-log builds); an optional `--expected-version` adds a caller assertion.
Production/unknown image versions are rejected before transfer. This option verifies upload and commit,
requests the existing dev flash endpoint once, then checks the same device ID
and exact new version for up to 120 seconds. A lost flash reply is not retried.
The new developer build consumes a one-shot boot marker and rejoins saved Wi-Fi;
missing credentials or association failure falls back to the reader's chooser.
Both current and new firmware must be developer builds for this loop; release
builds have no dev flash endpoint. This does not bootstrap an old release image,
survive an IP-address change automatically, or prove hardware acceptance.

Developer environments are now split: `default` keeps the update loop without
heavy network diagnostics; `network_diagnostics` opts into diagnostic buffers;
`sta_recovery` experiments with bounded Wi-Fi driver buffers. Do not treat the
experimental image as a verified production radio fix.

Do not use the Pocket Daily Sync hotspot for this on firmware older than
2026-09-03: its private AP left ~6 KB of heap and the companion's diagnostic
fetches tripped the task watchdog (`nearby:screen-preview`).

## Before handing off

- Did the build actually succeed? A stale `update.bin` from a previous run looks identical.
- Building a release image? Use the right env (`gh_release`, `gh_release_rc`) — the default
  env ships serial logging at `LOG_LEVEL=2`.
- Flashing is a hardware action with a failure mode you cannot see from here. State plainly
  which artifact you staged and let the user do the install and confirm.
