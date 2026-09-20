#!/usr/bin/env python3
"""Full-duplex upload probe: sends the payload while DRAINING every byte the
reader sends back, logging the reader's last words around the break point."""
import socket, sys, time, threading

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.68.68"
import uuid
PATH = sys.argv[2] if len(sys.argv) > 2 else f"/.pocket-{uuid.uuid4().hex[:32]}.part"
TOTAL = int(sys.argv[3]) if len(sys.argv) > 3 else 1024 * 1024
CHUNK = 16 * 1024

received = []
def reader(sock):
    sock.settimeout(0.5)
    while True:
        try:
            data = sock.recv(4096)
            if not data:
                received.append(("<closed>", time.time()))
                return
            received.append((data.decode(errors="replace"), time.time()))
        except socket.timeout:
            continue
        except OSError as e:
            received.append((f"<{type(e).__name__}>", time.time()))
            return

s = socket.create_connection((HOST, 82), timeout=8)
t = threading.Thread(target=reader, args=(s,), daemon=True)
t.start()

header = f"POCKET-PUT/1\nPath: {PATH}\nSize: {TOTAL}\n\n".encode()
s.sendall(header)
time.sleep(0.3)

payload = bytes([0x5A] * CHUNK)
sent = 0
t0 = time.time()
try:
    while sent < TOTAL:
        s.settimeout(30)
        s.sendall(payload)
        sent += CHUNK
        print(f"  {sent}/{TOTAL} (+{time.time()-t0:.1f}s)", flush=True)
except OSError as e:
    print(f"BREAK after {sent} bytes ({time.time()-t0:.1f}s): {type(e).__name__}: {e}", flush=True)

time.sleep(1.5)
print("--- reader said ---")
for msg, ts in received[-6:]:
    print(f"  [{ts-t0:+7.2f}s] {msg[:180]!r}")
