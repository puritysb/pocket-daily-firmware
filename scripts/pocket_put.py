#!/usr/bin/env python3
"""Push a file to a Pocket reader over the POCKET-PUT/1 stream and commit it.

Usage: pocket_put.py <file> [--target /update.bin] [--dir /] [--resume] [--host 192.168.4.1]
Prints every reader reply verbatim so a failure is attributable.
"""
import argparse, json, socket, sys, time, urllib.request, uuid, zlib

ap = argparse.ArgumentParser()
ap.add_argument("file")
ap.add_argument("--host", default="192.168.4.1")
ap.add_argument("--dir", default="/")
ap.add_argument("--target", default=None, help="published name (default: basename)")
ap.add_argument("--staging", default=None, help="reuse a staging name (for --resume)")
ap.add_argument("--resume", action="store_true")
args = ap.parse_args()

status = json.load(urllib.request.urlopen(f"http://{args.host}/api/status", timeout=6))
port = status.get("uploadStreamPort")
print("status:", json.dumps(status))
if not port:
    sys.exit("reader does not advertise uploadStreamPort")
if args.resume and not status.get("uploadStreamResume"):
    print("WARNING: reader does not advertise uploadStreamResume; header will be rejected by old firmware")

data = open(args.file, "rb").read()
total = len(data)
name = args.target or args.file.rsplit("/", 1)[-1]
staging = args.staging or f".pocket-{uuid.uuid4().hex}.part"
d = args.dir if args.dir != "/" else ""
staging_path, target_path = f"{d}/{staging}", f"{d}/{name}"
print(f"staging={staging_path} target={target_path} size={total}")

s = socket.create_connection((args.host, port), timeout=40)
s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
header = f"POCKET-PUT/1\nPath: {staging_path}\nSize: {total}\n" + ("Resume: 1\n" if args.resume else "") + "\n"
s.sendall(header.encode())

def read_line(sock):
    buf = b""
    while not buf.endswith(b"\n"):
        c = sock.recv(1)
        if not c:
            return buf.decode(errors="replace")
        buf += c
    return buf.decode(errors="replace").strip()

offset = 0
if args.resume:
    line = read_line(s)
    print("reader:", line)
    if not line.startswith("RESUME "):
        sys.exit("expected RESUME")
    offset = int(line.split()[1])

t0 = time.time()
sent = offset
CH = 16 * 1024
s.settimeout(40)
try:
    while sent < total:
        chunk = data[sent:sent + CH]
        s.sendall(chunk)
        sent += len(chunk)
        if sent % (256 * 1024) < CH or sent == total:
            el = time.time() - t0
            print(f"  {sent}/{total} ({sent*100//total}%) {((sent-offset)/1024)/max(el,0.001):.1f} KB/s", flush=True)
except (socket.timeout, OSError) as e:
    print(f"transport failure after {sent} bytes: {e}")
    try:
        s.settimeout(2)
        pending = s.recv(256).decode(errors="replace").strip()
        if pending:
            print("reader said:", pending)
    except OSError:
        pass
    print(f"retry with: --resume --staging {staging}")
    sys.exit(2)

line = read_line(s)
s.close()
print("reader:", line, f"({time.time()-t0:.1f}s)")
crc = zlib.crc32(data) & 0xFFFFFFFF
if not line.startswith("OK "):
    sys.exit(f"upload not verified; retry with --resume --staging {staging}")
_, rsize, rcrc = line.split()
if int(rsize) != total or int(rcrc, 16) != crc:
    sys.exit(f"CRC/size mismatch: local {total} {crc:08X}")

body = json.dumps({"staging": staging_path, "target": target_path, "size": total, "crc32": f"{crc:08X}"}).encode()
req = urllib.request.Request(f"http://{args.host}/api/pocket/v1/commit", data=body, headers={"Content-Type": "application/json"})
try:
    with urllib.request.urlopen(req, timeout=30) as r:
        print("commit:", r.status, r.read().decode())
except urllib.error.HTTPError as e:
    sys.exit(f"commit failed: {e.code} {e.read().decode()}")
print("published", target_path)
