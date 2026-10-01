#!/usr/bin/env python3
"""Summarise post-page-turn heap from a saved serial log (development build).

The firmware prints `[MEM] Free: … MaxAlloc: …` every 10 s and the reader
prints `Rendered page in …ms` per page. A sample is the first MEM line after
at least one rendered page, so turn a page, wait for the next MEM line, repeat.

    ./scripts/pio.sh device monitor -b 115200 | tee build/x3-baseline.log
    python3 scripts/heap_log_summary.py --skip 6 build/x3-baseline.log build/x3-change.log

This measures a device; it is evidence only for the image and book it ran on.
"""
import argparse
import re
import statistics
import sys
from pathlib import Path

MEM = re.compile(r"\[MEM\].*?Free: (\d+) bytes.*?MaxAlloc: (\d+) bytes")
PAGE = "Rendered page in"


def samples(lines):
    """(free, largest) of the first MEM line after each run of page renders."""
    out = []
    pages_since_mem = 0
    for line in lines:
        if PAGE in line:
            pages_since_mem += 1
            continue
        match = MEM.search(line)
        if not match or "PSRAM" in line:
            continue
        if pages_since_mem:
            out.append((int(match.group(1)), int(match.group(2))))
        pages_since_mem = 0
    return out


def summarise(values):
    return {"n": len(values), "median": statistics.median(values), "min": min(values), "max": max(values)}


def report(path, skip):
    taken = samples(Path(path).read_text(errors="replace").splitlines())[skip:]
    if not taken:
        return None
    return {"free": summarise([s[0] for s in taken]), "largest": summarise([s[1] for s in taken])}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("logs", nargs="+")
    parser.add_argument("--skip", type=int, default=0, help="warm-up samples to drop from each log")
    args = parser.parse_args(argv)
    results = []
    for path in args.logs:
        result = report(path, args.skip)
        if result is None:
            print(f"{path}: no post-page MEM samples (turn a page, then wait for a MEM line)")
            return 1
        results.append(result)
        for key in ("free", "largest"):
            s = result[key]
            print(f"{path}: {key:8} n={s['n']} median={s['median']:.0f} min={s['min']} max={s['max']}")
    if len(results) == 2:
        delta = results[1]["largest"]["median"] - results[0]["largest"]["median"]
        print(f"largest-block median delta (second - first): {delta:+.0f} B")
    return 0


if __name__ == "__main__":
    sys.exit(main())
