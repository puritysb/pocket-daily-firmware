#!/bin/bash
# One-shot wireless delivery over the reader's own hotspot.
# Runs autonomously: join hotspot -> push + retry -> remote flash ->
# cycle Wi-Fi so the Mac rejoins the preferred home network.
set -u
IP=192.168.4.1
FW="$(dirname "$0")/../firmware/update.bin"
LOG=/var/folders/qv/1jczm91d1sn45vtk0c46fxrw0000gn/T/opencode/deliver.log
exec >>"$LOG" 2>&1
echo "=== delivery $(date) ==="

networksetup -setairportnetwork en0 "CrossPoint-Reader"
sleep 5
echo "ip: $(ipconfig getifaddr en0)"

STAGE=""
for i in $(seq 1 60); do
  if [ -z "$STAGE" ]; then
    OUT=$(python3 "$(dirname "$0")/pocket_put.py" "$FW" --host $IP --target update.bin 2>&1 | tail -3)
  else
    OUT=$(python3 "$(dirname "$0")/pocket_put.py" "$FW" --host $IP --target update.bin --resume --staging "$STAGE" 2>&1 | tail -3)
  fi
  if echo "$OUT" | grep -q 'published'; then
    echo "PUSHED"
    break
  fi
  NEW=$(echo "$OUT" | grep -o '\.pocket-[a-f0-9]*\.part' | tail -1)
  [ -n "$NEW" ] && STAGE="$NEW"
  echo "attempt $i: $(echo "$OUT" | tail -1 | cut -c1-70)"
  sleep 2
done

if echo "$OUT" | grep -q 'published'; then
  curl -s --max-time 60 -X POST http://$IP/api/pocket/v1/dev/flash
  echo ""
  echo "FLASH SENT - reader reboots; will land in File Transfer menu"
  sleep 8
fi

# Restore home network
networksetup -setairportpower en0 off
sleep 2
networksetup -setairportpower en0 on
sleep 6
echo "back on: $(networksetup -getairportnetwork en0) ($(ipconfig getifaddr en0))"
echo "=== done ==="
