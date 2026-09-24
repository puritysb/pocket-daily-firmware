#!/usr/bin/env python3
"""Publish a file with bounded, identity-checked POCKET-PUT/1 recovery.

Publication is not firmware installation. Never retry an ambiguous commit.
"""
import argparse
import json
from pathlib import Path
import re
import socket
import sys
import time
import urllib.error
import urllib.request
import uuid
import zlib


class ProtocolError(Exception):
    """Terminal rejection: retrying would hide corruption or a contract error."""


def read_line(sock):
    buf = bytearray()
    while not buf.endswith(b"\n"):
        char = sock.recv(1)
        if not char:
            raise ConnectionError("reader disconnected before a complete reply")
        buf.extend(char)
        if len(buf) > 256:
            raise ProtocolError("reader reply exceeds 256 bytes")
    try:
        line = buf.decode("ascii").strip()
    except UnicodeDecodeError as error:
        raise ProtocolError("non-ASCII reader reply") from error
    if line in ("ERROR Upload timed out", "ERROR Upload disconnected"):
        raise ConnectionError(line)
    if line.startswith("ERROR"):
        raise ProtocolError(line)
    return line


def wait_for_reader(host, wait_seconds, expected_id=None):
    deadline = time.monotonic() + wait_seconds
    while True:
        try:
            with urllib.request.urlopen(f"http://{host}/api/status", timeout=3) as response:
                status = json.load(response)
            if not isinstance(status, dict):
                raise ProtocolError("status must be an object")
            if expected_id and status.get("deviceID") != expected_id:
                raise ProtocolError("reader identity changed; refusing to resume")
            port = status.get("uploadStreamPort")
            if type(port) is not int or not 1 <= port <= 65535:
                raise ProtocolError("reader does not advertise a valid uploadStreamPort")
            return status
        except urllib.error.HTTPError as error:
            raise ProtocolError(f"status rejected: HTTP {error.code}") from error
        except (ValueError, UnicodeDecodeError) as error:
            raise ProtocolError("malformed status response") from error
        except (urllib.error.URLError, TimeoutError, OSError):
            if time.monotonic() >= deadline:
                raise
            print("waiting for reader before upload…", flush=True)
            time.sleep(min(3, max(0, deadline - time.monotonic())))


def upload_once(host, status, data, staging_path, resume, legacy_delay_ms, legacy_chunk_bytes=4096, flow_delay_ms=0,
                progress=None):
    window = 4096 if status.get("uploadStreamWindow") == 4096 else None
    resume = resume or window is not None
    total = len(data)
    if not window and legacy_delay_ms:
        time.sleep(1)  # let the legacy HTTP connection drain
    with socket.create_connection((host, status["uploadStreamPort"]), timeout=40) as sock:
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        header = (f"POCKET-PUT/1\nPath: {staging_path}\nSize: {total}\n"
                  + ("Resume: 1\n" if resume else "")
                  + ("Window: 4096\n" if window else "") + "\n")
        sock.sendall(header.encode())
        offset = 0
        if resume:
            line = read_line(sock)
            print("reader:", line, flush=True)
            fields = line.split()
            if len(fields) != 2 or fields[0] != "RESUME" or not fields[1].isdigit():
                raise ProtocolError("expected RESUME offset")
            offset = int(fields[1])
            if not 0 <= offset <= total:
                raise ProtocolError("invalid resume offset")
        started = time.monotonic()
        send_seconds = 0.0
        reply_seconds = 0.0
        max_reply_seconds = 0.0
        reply_count = 0
        sent = offset
        acknowledged = offset if resume else None

        def report(phase):
            # Local observations only: no extra socket or reader status traffic.
            # Submitted bytes are not SD-accepted until a validated ACK/OK.
            if progress is not None:
                progress({'phase': phase, 'submitted_bytes': sent,
                          'acknowledged_bytes': acknowledged, 'resumed_bytes': offset,
                          'elapsed_seconds': time.monotonic() - started})

        report('ready')
        chunk_size = window or (legacy_chunk_bytes if legacy_delay_ms else 16 * 1024)
        while sent < total:
            chunk = data[sent:sent + chunk_size]
            send_started = time.monotonic()
            if window and flow_delay_ms:
                # Preserve the negotiated 4 KiB credit/ACK boundary, but avoid
                # bursting several full-sized Wi-Fi frames into a scarce RX heap.
                for start in range(0, len(chunk), 512):
                    sock.sendall(chunk[start:start + 512])
                    time.sleep(flow_delay_ms / 1000)
            else:
                sock.sendall(chunk)
            sent += len(chunk)
            send_seconds += time.monotonic() - send_started
            report('submitted')
            if window and sent < total:
                reply_started = time.monotonic()
                ack = read_line(sock)
                reply_duration = time.monotonic() - reply_started
                reply_seconds += reply_duration
                max_reply_seconds = max(max_reply_seconds, reply_duration)
                reply_count += 1
                if ack != f"ACK {sent}":
                    raise ProtocolError(f"expected ACK {sent}, got {ack!r}")
                acknowledged = sent
                report('acknowledged')
            elif not window and legacy_delay_ms:
                time.sleep(legacy_delay_ms / 1000)
            if sent % (256 * 1024) < chunk_size or sent == total:
                elapsed = time.monotonic() - started
                label = "SD acknowledged" if window and sent < total else "socket submitted"
                print(f"  {sent}/{total} ({label}) "
                      f"{((sent-offset)/1024)/max(elapsed, 0.001):.1f} KB/s", flush=True)
        reply_started = time.monotonic()
        line = read_line(sock)
        final_reply_seconds = time.monotonic() - reply_started
        print("reader:", line, flush=True)
        fields = line.split()
        try:
            valid = (len(fields) == 3 and fields[0] == "OK"
                     and int(fields[1]) == total
                     and int(fields[2], 16) == zlib.crc32(data) & 0xFFFFFFFF)
        except ValueError:
            valid = False
        if not valid:
            raise ProtocolError("upload size/CRC verification failed")
        acknowledged = total
        report('verified')
        return {"bytes": total, "resumed_bytes": offset, "sent_bytes": total - offset,
                "elapsed_seconds": time.monotonic() - started,
                "send_and_pacing_seconds": send_seconds,
                "credit_wait_seconds": reply_seconds, "credit_count": reply_count,
                "max_credit_wait_seconds": max_reply_seconds,
                "final_reply_seconds": final_reply_seconds}


def upload_with_recovery(args, data, staging_path):
    identity = None
    for attempt in range(args.attempts):
        status = wait_for_reader(args.host, args.wait_seconds, identity)
        if attempt == 0:
            identity = status.get("deviceID")
        elif not status.get("uploadStreamResume"):
            raise ProtocolError("reader no longer supports resume")
        print(f"attempt {attempt + 1}/{args.attempts}: version={status.get('version')} "
              f"freeHeap={status.get('freeHeap')}", flush=True)
        try:
            upload_once(args.host, status, data, staging_path,
                        args.resume or attempt > 0, args.legacy_delay_ms, args.legacy_chunk_bytes, args.flow_delay_ms)
            return status
        except (TimeoutError, OSError) as error:
            print(f"transport interrupted: {error}", flush=True)
            if (attempt + 1 == args.attempts or not isinstance(identity, str) or not identity
                    or not status.get("uploadStreamResume")):
                raise
            print("recovering the same reader and staging file…", flush=True)
            time.sleep(3)  # no competing status requests while the socket is being closed


def commit(host, staging_path, target_path, data):
    crc = f"{zlib.crc32(data) & 0xFFFFFFFF:08X}"
    body = json.dumps({"staging": staging_path, "target": target_path,
                       "size": len(data), "crc32": crc}).encode()
    request = urllib.request.Request(f"http://{host}/api/pocket/v1/commit", data=body,
                                     headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            result = json.load(response)
            if (not isinstance(result, dict) or result.get("size") != len(data)
                    or not isinstance(result.get("crc32"), str)
                    or result["crc32"].upper() != crc):
                raise ProtocolError("commit size/CRC mismatch")
    except urllib.error.HTTPError as error:
        raise ProtocolError(f"commit rejected: HTTP {error.code}") from error
    except (OSError, urllib.error.URLError, ValueError) as error:
        raise ProtocolError("commit outcome unknown; inspect target before retrying") from error
    print("published", target_path, "(publication does not install firmware)")


def flash_developer_image(host, previous_status, expected_version, timeout=120):
    """Explicit developer-only action; an ambiguous POST is never repeated."""
    identity = previous_status.get("deviceID")
    if not isinstance(identity, str) or not identity or previous_status.get("version") == expected_version:
        raise ProtocolError("dev flash requires an identified reader and a different exact build version")
    request = urllib.request.Request(f"http://{host}/api/pocket/v1/dev/flash", data=b"", method="POST")
    try:
        with urllib.request.urlopen(request, timeout=15) as response:
            response.read(256)
    except urllib.error.HTTPError as error:
        raise ProtocolError(f"dev flash rejected: HTTP {error.code}; production firmware has no dev endpoint") from error
    except (OSError, urllib.error.URLError):
        print("flash response lost; checking boot version without repeating the flash request", flush=True)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        time.sleep(3)
        try:
            status = wait_for_reader(host, 0, identity)
        except (OSError, urllib.error.URLError):
            continue
        if status.get("version") == expected_version:
            print("verified installed version:", expected_version)
            return
    raise ProtocolError("installation not verified before deadline; inspect reader (flash request was not repeated)")


def developer_version(data):
    # LOG_LEVEL=1 removes the startup debug line. The console identity and
    # HTTP user-agent still embed the same exact version in recovery builds.
    versions = set(re.findall(
        rb"(?:Starting CrossPoint version |CrossPoint version: |PocketDaily-ESP32-)"
        rb"([A-Za-z0-9._+\-]{1,160})(?:\r?\n)?\x00", data))
    if len(versions) != 1:
        raise ProtocolError("cannot uniquely identify the staged firmware build version")
    version = versions.pop().decode("ascii")
    if "-dev-" not in version:
        raise ProtocolError("--dev-flash requires a developer image, not a production image")
    return version


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("file")
    parser.add_argument("--host", default="192.168.4.1")
    parser.add_argument("--dir", default="/")
    parser.add_argument("--target", help="published basename")
    parser.add_argument("--staging", help="reuse a staging basename (for --resume)")
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--attempts", type=int, default=3, help="bounded bulk attempts (default: 3)")
    parser.add_argument("--legacy-delay-ms", type=float, default=0)
    parser.add_argument("--flow-delay-ms", type=float, default=0,
                        help="optional pacing per 512 bytes within a negotiated 4 KiB ACK window")
    parser.add_argument("--legacy-chunk-bytes", type=int, default=4096,
                        help="paced legacy write size, 256..16384 (does not change negotiated ACK windows)")
    parser.add_argument("--wait-seconds", type=float, default=45)
    parser.add_argument("--dev-flash", action="store_true",
                        help="explicitly flash /update.bin using a developer firmware endpoint after verified publication")
    parser.add_argument("--expected-version", help="optional extra check against the developer image's embedded version")
    args = parser.parse_args()
    if not 1 <= args.attempts <= 10 or not 0 <= args.wait_seconds <= 300 or not 0 <= args.legacy_delay_ms <= 1000:
        parser.error("attempts must be 1..10, wait seconds 0..300, legacy delay 0..1000 ms")
    if not 256 <= args.legacy_chunk_bytes <= 16384:
        parser.error("legacy chunk bytes must be 256..16384")
    if not 0 <= args.flow_delay_ms <= 1000:
        parser.error("flow delay must be 0..1000 ms")
    name = args.target or Path(args.file).name
    staging = args.staging or f".pocket-{uuid.uuid4().hex}.part"
    if any('/' in value or '\\' in value or any(ord(c) < 32 for c in value)
           or value in ("", ".", "..") for value in (name, staging)):
        parser.error("target and staging must be plain basenames")
    if (not args.dir.startswith('/') or any(ord(c) < 32 for c in args.dir)
            or '..' in args.dir.split('/') or '\\' in args.dir):
        parser.error("dir must be an absolute reader directory without traversal")
    directory = args.dir.rstrip('/')
    staging_path, target_path = f"{directory}/{staging}", f"{directory}/{name}"
    if staging_path == target_path:
        parser.error("staging and target must differ")
    if args.dev_flash and target_path != "/update.bin":
        parser.error("--dev-flash requires target /update.bin")
    try:
        data = Path(args.file).read_bytes()
        if args.dev_flash:
            version = developer_version(data)
            if args.expected_version and args.expected_version != version:
                raise ProtocolError("expected version does not match the image")
        print(f"staging={staging_path} target={target_path} size={len(data)}", flush=True)
        status = upload_with_recovery(args, data, staging_path)
        commit(args.host, staging_path, target_path, data)
        if args.dev_flash:
            flash_developer_image(args.host, status, version)
    except (ProtocolError, OSError, urllib.error.URLError) as error:
        print(str(error), file=sys.stderr)
        print(f"staging retained if supported: --resume --staging {staging}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
