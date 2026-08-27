#!/usr/bin/env python3
"""Fail when a v4 .cpfont omits codepoints required by a coverage file."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


HEADER = struct.Struct("<8sHHB19s")
STYLE_TOC = struct.Struct("<B3xIIBhhHHBBBI4x")
INTERVAL = struct.Struct("<III")


def load_required(path: Path) -> set[int]:
    required: set[int] = set()
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        lowered = line.lower()
        if lowered.startswith("u+"):
            required.add(int(line[2:], 16))
        elif lowered.startswith("0x"):
            required.add(int(line[2:], 16))
        else:
            try:
                required.add(int(line, 16))
            except ValueError:
                required.update(ord(ch) for ch in line)
    return required


def style_coverage(data: bytes, style_index: int) -> tuple[int, set[int]]:
    toc_offset = HEADER.size + style_index * STYLE_TOC.size
    if toc_offset + STYLE_TOC.size > len(data):
        raise ValueError(f"style {style_index}: truncated TOC")
    values = STYLE_TOC.unpack_from(data, toc_offset)
    style_id, interval_count, glyph_count = values[:3]
    data_offset = values[-1]
    covered: set[int] = set()
    counted_glyphs = 0
    for index in range(interval_count):
        offset = data_offset + index * INTERVAL.size
        if offset + INTERVAL.size > len(data):
            raise ValueError(f"style {style_id}: truncated interval table")
        first, last, glyph_offset = INTERVAL.unpack_from(data, offset)
        if first > last:
            raise ValueError(f"style {style_id}: reversed interval U+{first:04X}-U+{last:04X}")
        span = last - first + 1
        if glyph_offset + span > glyph_count:
            raise ValueError(f"style {style_id}: interval overruns glyph table")
        covered.update(range(first, last + 1))
        counted_glyphs += span
    if counted_glyphs != glyph_count:
        raise ValueError(
            f"style {style_id}: intervals cover {counted_glyphs} glyphs, header says {glyph_count}"
        )
    return style_id, covered


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("font", type=Path)
    parser.add_argument("coverage", type=Path)
    args = parser.parse_args()

    data = args.font.read_bytes()
    if len(data) < HEADER.size:
        raise SystemExit(f"{args.font}: truncated header")
    magic, version, _flags, style_count, _reserved = HEADER.unpack_from(data)
    if magic != b"CPFONT\0\0" or version != 4 or style_count == 0:
        raise SystemExit(
            f"{args.font}: expected cpfont v4 with at least one style "
            f"(magic={magic!r}, version={version}, styles={style_count})"
        )

    required = load_required(args.coverage)
    failed = False
    for index in range(style_count):
        style_id, covered = style_coverage(data, index)
        missing = sorted(required - covered)
        if missing:
            failed = True
            rendered = ", ".join(f"U+{cp:04X} {chr(cp)!r}" for cp in missing)
            print(f"style {style_id}: missing {len(missing)} codepoints: {rendered}")
        else:
            print(f"style {style_id}: covers all {len(required)} required codepoints")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
