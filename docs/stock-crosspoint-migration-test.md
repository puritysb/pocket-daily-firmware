# Stock CrossPoint 2.0 → Pocket Daily migration test

Hardware acceptance for the customer route in the app's `docs/OOBE.md`
(Route 1) and the first boot after stock CrossPoint
(`docs/upstream-integration-2.0.0.md`, "Moving from stock CrossPoint"). It
installs official stock CrossPoint 2.0.0 on a reader that runs Pocket Daily,
then moves it back the way a customer would. Prepared and run on the X4 on
2026-11-06; results below.

## Inputs

| Item | Value |
| --- | --- |
| Reader | X4, device ID `C8729208` (X3 follows the same steps) |
| Stock image | `crosspoint-2.0.0-x3-x4.bin` from the upstream `2.0.0` release, 5,537,104 B, SHA-256 `56d27fec71c6b6ec62a1bcf508cefa80805b4a63ff6eb8793ae754bfb5f8a877` (matches the release digest), board tag `x4` |
| Pocket Daily image | developer build from `main`; prepared 2026-11-06: `1.0.0-dev-main-c3e2f5b3-w00de6c32`, 6,238,688 B, SHA-256 `1f17bbfba8ddca2778239df8f025b9bec04c63a4c06f72590b16f24c747aa0fb` |
| App | Debug PocketMac built ad hoc without entitlements (`CODE_SIGN_IDENTITY=- CODE_SIGN_ENTITLEMENTS=`), launched with `open -n --env POCKET_DEBUG_FIRMWARE_RELEASE=<image>`. The signed app is sandboxed and cannot read the image; this host has no Simulator app. Separate defaults, so the session starts fresh. |
| Evidence | `build/stock-2.0.0-<date>/` (private; holds device state) |

## Why not the developer flash

`POST /api/pocket/v1/dev/flash` would accept the stock image (it validates
structure only), but it leaves `/.crosspoint/dev-boot-file-transfer` for its
own return. Stock never consumes it, so the next Pocket Daily boot would take
the developer return branch and skip the first-boot-after-CrossPoint path under
test. Install stock through the reader's own SD Card Firmware Update instead.

## Steps

`[agent]` steps run over Same Wi-Fi; `[owner]` steps need the reader's buttons.

1. `[agent]` Baseline in Sync → Same Wi-Fi: `/api/status`, `/api/settings`,
   `/api/fonts`, reader file inventory. Note the open book and its page.
2. `[agent]` Make the card look like a stock customer's: delete the
   `PocketSansWorld` family (restored in step 11; local copy SHA-256
   `a1a15a6e…ee753`). `PocketSansJP` is reinstalled by Pocket Daily at boot.
3. `[agent]` Publish the stock image to the SD root as
   `crosspoint-2.0.0-x3-x4.bin` with `scripts/pocket_put.py`; confirm the
   reader-side CRC.
4. `[owner]` Back out of Sync → Home → Settings → System → SD Card Firmware
   Update → `crosspoint-2.0.0-x3-x4.bin` → Confirm. The reader restarts into
   stock CrossPoint 2.0.0. Stock resaves `settings.json` without the fork keys.
5. `[owner]` Stock: File Transfer → Join a Network (reuses the saved Wi-Fi).
   `[agent]` `/api/status` reports `2.0.0`, `firmwareFamily` `crosspoint`, no
   `deviceID`; record free heap and largest block.
6. `[owner]` App (fresh session): My Reader → Find on Same Wi-Fi. Expect the
   CrossPoint card. Manage Reader → Firmware → Install Pocket Daily… → Copy to
   Reader. Expect the read-back to match and the new staged text ("Pocket Daily
   opens Sync by itself and this app reconnects"). Keep the app open.
7. `[owner]` Reader: Back → Settings → System → SD Card Firmware Update →
   `pocket-daily.bin` → Confirm.
8. Observe, without touching the reader: it boots into Pocket Sync, Same
   Wi-Fi, Back labelled "Read offline"; the app reconnects within about 30 s
   and names the reader as Pocket Daily. `[agent]` `/api/status` reports the
   staged version and the same device ID.
9. `[agent]` Carry-over: saved Wi-Fi (it joined), `/api/settings` against step
   1 (stock-only values may reset; line spacing and font family kept), the
   book from step 1 opens at its page.
10. `[owner]` Leave Sync, then power the reader off and on (a developer
    restart takes its own return branch and proves nothing). Sync must not
    open by itself: the first load wrote the fork marker.
11. `[agent]` Restore `PocketSansWorld` (or use the app's Install Card Font,
    which exercises that step too) and confirm the CRC.

Optional, needs a second stock round: repeat steps 3–7 without opening the app
and confirm the reader returns to reading after five minutes.

## Stop and recover

- Copy fails in step 6 (stock File Transfer ran out of memory on 1.6.5): retry
  once; otherwise copy the Pocket Daily image to the SD root with a card reader
  (Route 2) and continue at step 7.
- Reader does not boot after either install: the previous image stays in the
  other OTA slot only until the next install; recover over USB with the
  repository uploader (app `docs/OOBE.md`, Route 3).
- Stock cannot see Wi-Fi: use stock's Wi-Fi settings; the saved network list
  is shared.

## Results — X4, 2026-11-06

| Step | Result |
| --- | --- |
| 1–3 | Baseline recorded; `PocketSansWorld` removed; stock image on the SD root, reader CRC matched. |
| 4–5 | Stock 2.0.0 installed through the reader's SD Card Firmware Update. `/api/status`: `2.0.0`, `firmwareFamily` `crosspoint`, no `deviceID`; File Transfer free heap about 69–73 KB. |
| 6 | First app attempt failed: the Debug app's registry would not load (a host test had left the fixture ID `install-reader` in the unsandboxed app's defaults). After moving the app state aside, the app found the CrossPoint reader and copied `pocket-daily.bin`; read-back matched. Fixed in app PR #29 (registry sanitizing, Reset Saved Readers, test isolation). |
| 7–8 | The reader installed the image and answered at 18:34:19 with the staged version and the same device ID `C8729208`. It joined the saved Wi-Fi and opened Sync by itself; Back read "Read offline" (owner). Sync stayed open past five minutes, so the app was making Pocket requests (the reconnect). Free heap 38,408, largest block 34,804. |
| 9 | Reading positions unchanged. Settings unchanged except `fontFamily` 0 → 1: stock had cleared the missing SD `NotoSerif` choice on its own load, which is expected. |
| 10 | A sleep/wake returned to Home, but a power-button wake is excluded from the first-boot branch by design (`isSleepWake`), and the X4's power-off is a sleep. Checked the file instead: `/.crosspoint/settings.json` holds `startupApp` and `longPressMenuOrder`, so the branch cannot repeat. |
| 11 | `PocketSansWorld` restored (CRC `A652D7ED`). The three install images were deleted from the SD root afterwards. |

Evidence is in `build/stock-2.0.0-20261106/` (private). Not run: the optional
five-minute exit without the app, and a cold boot (only USB or a reset gives
one on the X4).

## Not covered

X3, all four orientations, a card that never ran Pocket Daily (Pocket state
files from earlier use remain on this card), stock CrossPoint 1.x.
