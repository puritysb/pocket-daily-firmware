#!/usr/bin/env python3
"""Build a .uipack theme-override pack from a JSON description.

Input: {"name": "...", "version": "...", "theme": {"fieldName": value, ...}}
Field names resolve through scripts/theme_fields.json (generated from
BaseTheme.h). Output layout: docs/live-studio-v1.md / src/pocket_daily/
live_studio/UiPack.h. SHA-256 over the payload is embedded via hashlib.
"""

import hashlib
import json
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
FIELDS = {f["name"]: f for f in json.loads((REPO / "scripts/theme_fields.json").read_text())["fields"]}


def build(description: dict) -> bytes:
    name = description.get("name", "pack")[:32].encode()
    version = description.get("version", "1.0")[:16].encode()
    min_fw = description.get("minFirmware", "")[:16].encode()
    theme = description.get("theme", {})

    field_names = list(FIELDS)
    payload = bytearray()
    for field_name, value in theme.items():
        field = FIELDS.get(field_name)
        if field is None:
            sys.exit(f"unknown theme field: {field_name}")
        t = field["type"]
        if t == 1:
            bits = struct.pack("<i", int(value))
        elif t == 2:
            bits = struct.pack("<i", 1 if value else 0)
        else:
            bits = struct.pack("<f", float(value))
        payload += struct.pack("<HB", field_names.index(field_name), t) + bits

    crc = 0xFFFFFFFF
    for b in payload:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1))
    crc = ~crc & 0xFFFFFFFF
    sha = hashlib.sha256(bytes(payload)).digest()

    header = bytearray(100)
    header[0:4] = b"PDUI"
    header[4] = 1
    header[8:8 + len(name)] = name
    header[40:40 + len(version)] = version
    header[56:56 + len(min_fw)] = min_fw
    struct.pack_into("<H", header, 64, len(theme))
    struct.pack_into("<I", header, 72, len(payload))
    struct.pack_into("<I", header, 76, crc)
    header[80:112] = sha
    return bytes(header) + bytes(payload)


def main():
    if len(sys.argv) != 3:
        sys.exit(f"usage: {sys.argv[0]} description.json output.uipack")
    description = json.loads(Path(sys.argv[1]).read_text())
    blob = build(description)
    Path(sys.argv[2]).write_bytes(blob)
    print(f"wrote {sys.argv[2]} ({len(blob)} bytes, {len(description.get('theme', {}))} overrides)")


if __name__ == "__main__":
    main()
