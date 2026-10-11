#!/usr/bin/env python3
"""Run explicit developer BLE sleep-path cycles and verify automatic LAN return.

Does not change the Mac network. A cycle exercises terminal display release and
BLE, then software-reboots by default. --timer-sleep tests real X3 deep sleep
with a three-second timer wake. Neither mode measures current.
Never retries an ambiguous POST or installation. Evidence belongs in ignored build/.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import ipaddress
import json
from pathlib import Path
import secrets
import subprocess
import sys
import time
import urllib.error
import urllib.request

import pocket_put


def get_json(host, path, timeout=3):
    with urllib.request.urlopen(f"http://{host}{path}", timeout=timeout) as response:
        return json.load(response)


def verify_identity(status, identity, version=None):
    if not isinstance(status, dict) or status.get("deviceID") != identity:
        raise pocket_put.ProtocolError("reader identity changed")
    if version is not None and status.get("version") != version:
        raise pocket_put.ProtocolError("unexpected firmware version")


def discover(subnet, identity):
    network = ipaddress.ip_network(subnet, strict=False)
    if network.version != 4 or not network.is_private or network.num_addresses > 256:
        raise ValueError("discovery must be a private IPv4 subnet of at most 256 addresses")

    def probe(address):
        host = str(address)
        try:
            status = get_json(host, "/api/status", 1)
            if isinstance(status, dict) and status.get("deviceID") == identity:
                return host
        except (OSError, ValueError):
            pass
        return None

    with ThreadPoolExecutor(max_workers=8) as pool:
        matches = [host for host in pool.map(probe, network.hosts()) if host]
    if len(matches) > 1:
        raise pocket_put.ProtocolError("ambiguous reader identity on LAN")
    return matches[0] if matches else None


def start_cycle(host, run, timer_sleep=False, standby=False):
    query = f"run={run}" + ("&sleep=standby" if standby else "&sleep=timer" if timer_sleep else "")
    request = urllib.request.Request(f"http://{host}/api/pocket/v1/dev/ble-cycle?{query}",
                                     data=b"", method="POST")
    try:
        with urllib.request.urlopen(request, timeout=8) as response:
            if response.status != 202:
                raise pocket_put.ProtocolError("cycle request was not accepted")
            response.read(256)
    except urllib.error.HTTPError as error:
        raise pocket_put.ProtocolError(f"cycle rejected: HTTP {error.code}") from error
    except (OSError, urllib.error.URLError):
        print("cycle reply lost; observing the same run without repeating POST", flush=True)


def check_result(record, require_exchange=False, timer_sleep=False, standby=False):
    failures = []
    if record.get("state") != 3:
        failures.append("cycle did not complete")
    if not record.get("frameReleased"):
        failures.append("framebuffer not released")
    frame_bytes = record.get("frameBytes", 0)
    if frame_bytes <= 0 or record.get("afterFree", 0) - record.get("beforeFree", 0) < frame_bytes:
        failures.append("framebuffer bytes were not recovered")
    if not record.get("statsValid") or record.get("gate") != "open" or record.get("opened", 0) < 1:
        failures.append("BLE window did not open")
    if record.get("minFree", 0) <= 50 * 1024 or record.get("minBlock", 0) < 8 * 1024:
        failures.append("runtime heap target not met")
    if require_exchange and (record.get("connections", 0) < 1 or record.get("lists", 0) < 1):
        failures.append("no completed phone/Mac reading-list exchange")
    if timer_sleep and (record.get("returnMode") != 1 or not record.get("timerArmed") or not record.get("timerWake")):
        failures.append("real timer sleep/wake was not verified")
    if standby and (not record.get("fromWifi") or record.get("returnMode") != 2 or record.get("close") != "app-wifi"
                    or record.get("lightSleeps", 0) < 1 or record.get("lightSleepMs", 0) < 1):
        failures.append("authenticated app wake and actual light sleep were not verified")
    if failures:
        raise pocket_put.ProtocolError("; ".join(failures))


def wait_cycle(host, identity, version, run, timeout, subnet=None):
    deadline = time.monotonic() + timeout
    next_discovery = time.monotonic() + 45
    while time.monotonic() < deadline:
        time.sleep(3)
        try:
            status = get_json(host, "/api/status")
            verify_identity(status, identity, version)
            record = get_json(host, "/api/pocket/v1/dev/ble-cycle")
            if not isinstance(record, dict):
                raise pocket_put.ProtocolError("malformed cycle evidence")
            if record.get("run") == run and record.get("state") in (3, 4):
                return host, status, record
        except urllib.error.HTTPError as error:
            if error.code != 404:
                raise
        except (OSError, ValueError):
            if subnet and time.monotonic() >= next_discovery:
                found = discover(subnet, identity)
                if found:
                    host = found
                next_discovery = time.monotonic() + 45
    raise pocket_put.ProtocolError("cycle outcome unverified; POST was not repeated")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--subnet", help="optional private /24 or smaller for changed-IP recovery")
    parser.add_argument("--cycles", type=int, default=1)
    parser.add_argument("--timeout", type=int, default=150)
    parser.add_argument("--require-exchange", action="store_true")
    parser.add_argument("--timer-sleep", action="store_true", help="X3 only: actual deep sleep and timer wake")
    parser.add_argument("--standby", action="store_true", help="test app-requested BLE standby wake; connect from the companion during the trial")
    parser.add_argument("--interval", type=int, default=60,
                        help="seconds between cycles; default respects the app's 60-second BLE cooldown")
    parser.add_argument("--firmware", type=Path, help="explicitly install this developer image before cycles")
    parser.add_argument("--build", action="store_true", help="run host/route/pipeline tests, strict analysis and default build first")
    parser.add_argument("--cppcheck", default="build/cppcheck-source/cppcheck", help="cppcheck 2.11 for --build")
    parser.add_argument("--output", type=Path, default=Path("build/ble-cycle-runs"))
    args = parser.parse_args()
    if not 1 <= args.cycles <= 100 or not 30 <= args.timeout <= 600 or not 0 <= args.interval <= 300:
        parser.error("cycles must be 1..100, timeout 30..600 and interval 0..300 seconds")
    if args.build and args.firmware:
        parser.error("use --build or --firmware, not both")
    if args.standby and args.timer_sleep:
        parser.error("standby and timer-sleep are different experiments")
    if args.standby and args.timeout == 150:
        args.timeout = 240
    host = args.host
    status = get_json(host, "/api/status")
    identity, version = status.get("deviceID"), status.get("version", "")
    if not identity or "-dev-" not in version or status.get("mode") != "STA":
        raise pocket_put.ProtocolError("requires an identified developer reader on Same Wi-Fi")
    args.output.mkdir(parents=True, exist_ok=True)
    if args.build:
        root = Path(__file__).resolve().parents[1]
        commands = [
            ["cmake", "-S", "test", "-B", "build/host-tests", "-DCMAKE_BUILD_TYPE=Release"],
            ["cmake", "--build", "build/host-tests", "-j8"],
            ["ctest", "--test-dir", "build/host-tests", "--output-on-failure", "-j8"],
            *[[sys.executable, "-m", "unittest", "discover", "-s", "scripts", "-p", pattern]
              for pattern in ("test_dev_ble_cycle.py", "test_pocket_put.py", "test_sync_routes.py")],
            [sys.executable, "scripts/check_firmware.py", "--cppcheck", args.cppcheck],
            ["./scripts/pio.sh", "run", "-e", "default"],
            *([["./scripts/pio_ble_standby.sh"]] if args.standby else []),
        ]
        for command in commands:
            subprocess.run(command, cwd=root, check=True)
        args.firmware = root / "firmware/update.bin"
    if args.firmware:
        candidate = pocket_put.developer_version(args.firmware.read_bytes())
        if candidate == version:
            print("exact candidate already installed; skipping installation", flush=True)
        else:
            command = [sys.executable, str(Path(__file__).with_name("pocket_put.py")), str(args.firmware),
                       "--host", host, "--target", "update.bin", "--resume", "--dev-flash"]
            outcome = subprocess.run(command, check=False)
            # After an ambiguous result, only inspect; never repeat flashing.
            if outcome.returncode and args.subnet:
                host = discover(args.subnet, identity) or host
            status = get_json(host, "/api/status")
            verify_identity(status, identity, candidate)
            version = candidate
    for index in range(args.cycles):
        if index:
            print(f"waiting {args.interval}s for the companion's normal reconnect cooldown", flush=True)
            for offset in range(0, args.interval, 30):
                time.sleep(min(30, args.interval - offset))
        run = secrets.randbelow(0xffffffff) + 1
        status = get_json(host, "/api/status")
        verify_identity(status, identity, version)
        if status.get("mode") != "STA":
            raise pocket_put.ProtocolError("reader left Same Wi-Fi")
        print(f"cycle {index + 1}/{args.cycles}: run={run}", flush=True)
        start_cycle(host, run, args.timer_sleep, args.standby)
        host, status, record = wait_cycle(host, identity, version, run, args.timeout, args.subnet)
        # Save failed evidence too, before assertions. Device IDs remain local.
        (args.output / f"{run}.json").write_text(json.dumps(
            {"version": version, "status": status, "cycle": record}, indent=2) + "\n")
        print(json.dumps(record, sort_keys=True), flush=True)
        check_result(record, args.require_exchange, args.timer_sleep, args.standby)
        if args.timer_sleep and status.get("lastResetReason") != "deep-sleep wake":
            raise pocket_put.ProtocolError("reader did not report a real deep-sleep reset")
        print("verified memory/radio cycle and automatic Same Wi-Fi return", flush=True)
    mode = "app-requested BLE standby wake" if args.standby else "timer sleep/wake" if args.timer_sleep else "software return (no physical deep sleep)"
    print(f"completed {args.cycles} {mode} cycles; current was not measured", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (pocket_put.ProtocolError, OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"FAILED: {error}", file=sys.stderr)
        sys.exit(1)
