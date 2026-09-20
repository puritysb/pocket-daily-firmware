#!/bin/bash
# Persistent background delivery: probes the reader and pushes+resumes the
# firmware whenever the radio is reachable (flickers count). Accumulates the
# verified prefix on the reader's SD staging file across recoveries.
set -u
FW="$(cd "$(dirname "$0")/.." && pwd)/firmware/update.bin"
LOG=/var/folders/qv/1jczm91d1sn45vtk0c46fxrw0000gn/T/opencode/delivery-daemon.log
exec >>"$LOG" 2>&1
echo "=== daemon start $(date) (auto-discovery) ==="

# Find any live reader on the subnet (leases move); ~1 s per sweep.
discover() {
  python3 - <<'PY'
import concurrent.futures, urllib.request
def p(ip):
    try:
        urllib.request.urlopen(f"http://{ip}/api/status", timeout=0.8).read(64)
        return ip
    except Exception:
        return None
with concurrent.futures.ThreadPoolExecutor(64) as ex:
    for res in ex.map(p, [f"192.168.68.{i}" for i in range(1, 255)]):
        if res:
            print(res)
            break
PY
}

STAGE=""
while true; do
  IP=$(discover)
  if [ -z "$IP" ]; then
    sleep 4
    continue
  fi
  if [ -z "$STAGE" ]; then
    OUT=$(timeout 400 python3 "$(dirname "$0")/pocket_put.py" "$FW" --host $IP --target update.bin 2>&1 | tail -3)
  else
    OUT=$(timeout 400 python3 "$(dirname "$0")/pocket_put.py" "$FW" --host $IP --target update.bin --resume --staging "$STAGE" 2>&1 | tail -3)
  fi
  echo "$(date +%H:%M:%S) $(echo "$OUT" | tail -1 | cut -c1-90)"
  if echo "$OUT" | grep -q 'published'; then
    echo "PUSHED - flashing"
    curl -s --max-time 60 -X POST http://$IP/api/pocket/v1/dev/flash
    echo ""
    echo "FLASH SENT $(date)"
    exit 0
  fi
  NEW=$(echo "$OUT" | grep -o '\.pocket-[a-f0-9]*\.part' | tail -1)
  [ -n "$NEW" ] && STAGE="$NEW"
  sleep 3
done
