#!/usr/bin/env bash
# Push a firmware image to a device's SD card over the File Transfer web server.
#
# This is the fallback delivery path for a unit whose WiFi link is too lossy for
# the 5.4 MB AgentDeck WS OTA (`agentdeck esp32-ota xteink_x3 --firmware ...`).
# The web upload is a single HTTP POST: TCP retransmits absorb the packet loss
# that makes the chunk/ack OTA protocol time out, so it survives a link that
# OTA cannot.
#
# The device must already be in File Transfer mode — this script cannot press
# its buttons:
#
#   Home → File Transfer → Join a Network   (STA: stays on the LAN)
#          … or → Create Hotspot            (AP: join the device's own SSID
#                                            from this Mac; strongest link,
#                                            bypasses the router entirely)
#
# The screen shows the IP to pass here. After the upload, flash it on-device:
#
#   Settings → System → Update firmware      (or hold UP + POWER at boot)
#
# Usage: scripts/push-firmware-http.sh <device-ip> [firmware.bin]
set -euo pipefail

IP="${1:-}"
FW="${2:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/firmware/update.bin}"

if [[ -z "$IP" ]]; then
  echo "usage: $0 <device-ip> [firmware.bin]" >&2
  echo "  the IP is shown on the device's File Transfer screen" >&2
  exit 2
fi
if [[ ! -f "$FW" ]]; then
  echo "no firmware at $FW" >&2
  exit 2
fi

SIZE=$(stat -f%z "$FW" 2>/dev/null || stat -c%s "$FW")
echo "firmware : $FW"
echo "size     : $SIZE bytes"
command -v md5sum >/dev/null 2>&1 && echo "md5      : $(md5sum "$FW" | cut -d' ' -f1)" \
  || echo "md5      : $(md5 -q "$FW")"

# Confirm the target really is in File Transfer mode before pushing 5 MB at it.
echo
echo "probing http://$IP/api/status ..."
STATUS=$(curl -fsS --connect-timeout 5 --max-time 10 "http://$IP/api/status" 2>/dev/null || true)
if [[ -z "$STATUS" ]]; then
  echo "no web server at $IP." >&2
  echo "the device is not in File Transfer mode, or the IP is wrong." >&2
  exit 1
fi
echo "device   : $STATUS"

# RSSI is worth reading out loud here: this path exists because of a weak link,
# and a bad number predicts a slow (or stalling) upload.
echo
echo "uploading to SD root as update.bin (this takes a minute on a weak link) ..."
RESPONSE=$(curl -sS --fail-with-body \
  --connect-timeout 10 --max-time 900 \
  --progress-bar \
  -F "file=@$FW;filename=update.bin" \
  "http://$IP/upload?path=/" 2>&1) || {
  echo
  echo "upload failed: $RESPONSE" >&2
  echo "the running firmware is untouched, but update.bin on SD may be incomplete." >&2
  echo "do not flash it; reopen File Transfer and retry until size verification passes." >&2
  exit 1
}
echo "server   : $RESPONSE"

# Verify the landed size before anyone flashes it. A truncated image is the one
# outcome that turns a failed transfer into a bricked boot.
echo
echo "verifying on-device size ..."
LANDED=$(curl -fsS --connect-timeout 5 --max-time 30 "http://$IP/api/files?path=/" 2>/dev/null \
  | tr ',' '\n' | grep -A1 '"update.bin"' | grep -o '"size":[0-9]*' | head -1 | cut -d: -f2 || true)
if [[ -z "$LANDED" ]]; then
  echo "could not read back the file list — check the device's file browser by hand." >&2
elif [[ "$LANDED" != "$SIZE" ]]; then
  echo "SIZE MISMATCH: on-device $LANDED != local $SIZE — do NOT flash this." >&2
  echo "re-run the upload." >&2
  exit 1
else
  echo "ok       : $LANDED bytes on device, matches local"
fi

cat <<EOF

next, on the device:
  Settings → System → Update firmware   (or hold UP + POWER at boot, pick update.bin)

after reboot, open Pocket Daily and check the build shown by the Pocket app.
AgentDeck is not required for firmware installation or verification.
EOF
