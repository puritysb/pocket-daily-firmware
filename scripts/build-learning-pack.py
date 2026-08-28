#!/usr/bin/env python3
"""Build and verify a deterministic Pocket Daily offline learning pack."""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SOURCE = ROOT / "learning/content/jp-n3-ko.json"
DEFAULT_OUTPUT = ROOT / "learning/dist/jp-n3-ko.pdl"
HEADER = struct.Struct("<4sHHHHIII32s16s48s32s40s160s32sI")
RECORD = struct.Struct("<IBBH8s64s64s96s96s48s64s96s192s192s")
MAGIC = b"PDLP"
FORMAT_VERSION = 1
ALLOWED_CONTENT_LICENSES = {"CC0-1.0", "CC-BY-4.0", "CC-BY-SA-4.0"}


def fixed(value: str, size: int, field: str) -> bytes:
    encoded = value.encode("utf-8")
    if not value or len(encoded) >= size:
        raise ValueError(f"{field}: must contain 1..{size - 1} UTF-8 bytes")
    return encoded + bytes(size - len(encoded))


def optional_fixed(value: str, size: int, field: str) -> bytes:
    encoded = value.encode("utf-8")
    if len(encoded) >= size:
        raise ValueError(f"{field}: must contain at most {size - 1} UTF-8 bytes")
    return encoded + bytes(size - len(encoded))


def fnv32(data: bytes) -> int:
    value = 2166136261
    for byte in data:
        value ^= byte
        value = (value * 16777619) & 0xFFFFFFFF
    return value


def stable_item_id(glyph: str) -> int:
    value = fnv32(glyph.encode("utf-8"))
    return value or 1


def require_object(value: Any, field: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ValueError(f"{field}: expected object")
    return value


def build(source: Path, output: Path) -> None:
    root = require_object(json.loads(source.read_text(encoding="utf-8")), "root")
    package = require_object(root.get("package"), "package")
    records = root.get("records")
    if not isinstance(records, list) or not records:
        raise ValueError("records: expected a non-empty array")

    package_id = str(package.get("id", ""))
    if package_id != "jp-n3-ko":
        raise ValueError("package.id: device contract requires 'jp-n3-ko'")
    license_spdx = str(package.get("licenseSpdx", ""))
    attribution = str(package.get("attribution", ""))
    source_revision = str(package.get("sourceRevision", ""))
    if not license_spdx or not attribution or not source_revision:
        raise ValueError("package: licenseSpdx, attribution and sourceRevision are mandatory")
    if license_spdx not in ALLOWED_CONTENT_LICENSES:
        raise ValueError(f"package.licenseSpdx: unsupported content license {license_spdx!r}")
    sources = package.get("sources")
    if not isinstance(sources, list) or not sources:
        raise ValueError("package.sources: a non-empty source and licence ledger is mandatory")
    for index, raw_source in enumerate(sources):
        source_entry = require_object(raw_source, f"package.sources[{index}]")
        for field in ("name", "url", "revision", "licenseSpdx", "attribution"):
            if not str(source_entry.get(field, "")).strip():
                raise ValueError(f"package.sources[{index}].{field}: mandatory")
        if source_entry["licenseSpdx"] not in ALLOWED_CONTENT_LICENSES:
            raise ValueError(
                f"package.sources[{index}].licenseSpdx: unsupported content license "
                f"{source_entry['licenseSpdx']!r}"
            )

    payload = bytearray()
    item_ids: set[int] = set()
    glyphs: set[str] = set()
    for index, raw in enumerate(records):
        row = require_object(raw, f"records[{index}]")
        glyph = str(row.get("glyph", ""))
        item_id = int(row.get("itemId") or stable_item_id(glyph))
        if item_id in item_ids:
            raise ValueError(f"records[{index}].itemId: duplicate {item_id}")
        if glyph in glyphs:
            raise ValueError(f"records[{index}].glyph: duplicate {glyph!r}")
        item_ids.add(item_id)
        glyphs.add(glyph)
        level = int(row.get("level", 3))
        if level < 1 or level > 5:
            raise ValueError(f"records[{index}].level: expected 1..5")
        card_context = (
            f"{row.get('primaryWord', '')}（{row.get('wordReading', '')}） · "
            f"{row.get('meaningEn', '')} · {row.get('example', '')}"
        )
        if len(card_context.encode("utf-8")) >= 192:
            raise ValueError(
                f"records[{index}]: compact Today-card context exceeds 191 UTF-8 bytes"
            )
        payload.extend(
            RECORD.pack(
                item_id,
                level,
                int(row.get("flags", 0)),
                0,
                fixed(glyph, 8, f"records[{index}].glyph"),
                optional_fixed(str(row.get("on", "")), 64, f"records[{index}].on"),
                optional_fixed(str(row.get("kun", "")), 64, f"records[{index}].kun"),
                fixed(str(row.get("meaningKo", "")), 96, f"records[{index}].meaningKo"),
                fixed(str(row.get("meaningEn", "")), 96, f"records[{index}].meaningEn"),
                fixed(str(row.get("primaryWord", "")), 48, f"records[{index}].primaryWord"),
                fixed(str(row.get("wordReading", "")), 64, f"records[{index}].wordReading"),
                fixed(str(row.get("wordMeaningKo", "")), 96, f"records[{index}].wordMeaningKo"),
                fixed(str(row.get("example", "")), 192, f"records[{index}].example"),
                fixed(
                    str(row.get("exampleMeaningKo", "")),
                    192,
                    f"records[{index}].exampleMeaningKo",
                ),
            )
        )

    content_version = int(package.get("contentVersion", 0))
    if content_version <= 0:
        raise ValueError("package.contentVersion: expected a positive integer")
    total_bytes = HEADER.size + len(payload)
    payload_sha = hashlib.sha256(payload).digest()
    header_without_hash = HEADER.pack(
        MAGIC,
        FORMAT_VERSION,
        HEADER.size,
        RECORD.size,
        0,
        len(records),
        content_version,
        total_bytes,
        fixed(package_id, 32, "package.id"),
        fixed(str(package.get("locale", "")), 16, "package.locale"),
        fixed(str(package.get("title", "")), 48, "package.title"),
        fixed(license_spdx, 32, "package.licenseSpdx"),
        fixed(source_revision, 40, "package.sourceRevision"),
        fixed(attribution, 160, "package.attribution"),
        payload_sha,
        0,
    )
    header_hash = fnv32(header_without_hash[:-4])
    header = header_without_hash[:-4] + struct.pack("<I", header_hash)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(output.suffix + ".tmp")
    temporary.write_bytes(header + payload)
    temporary.replace(output)

    verify(output)
    file_md5 = hashlib.md5(output.read_bytes(), usedforsecurity=False).hexdigest()
    manifest = {
        "learningPack": {
            "id": package_id,
            "version": content_version,
            "format": FORMAT_VERSION,
            "size": total_bytes,
            "md5": file_md5,
            "licenseSpdx": license_spdx,
        },
        "attribution": attribution,
        "sources": sources,
    }
    manifest_path = output.with_suffix(".manifest.json")
    manifest_path.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    try:
        display_path = output.resolve().relative_to(ROOT)
    except ValueError:
        display_path = output
    print(f"{display_path}: {len(records)} records, {total_bytes} bytes, md5={file_md5}")


def verify(path: Path) -> None:
    data = path.read_bytes()
    if len(data) < HEADER.size:
        raise ValueError("pack is shorter than its header")
    values = HEADER.unpack_from(data)
    magic, version, header_size, record_size = values[:4]
    record_count, content_version, total_bytes = values[5:8]
    payload_sha, header_hash = values[-2:]
    if magic != MAGIC or version != FORMAT_VERSION:
        raise ValueError("pack magic/version mismatch")
    if header_size != HEADER.size or record_size != RECORD.size:
        raise ValueError("pack structure size mismatch")
    if total_bytes != len(data) or HEADER.size + record_count * RECORD.size != len(data):
        raise ValueError("pack file size mismatch")
    if content_version <= 0 or record_count <= 0:
        raise ValueError("pack has no content revision or records")
    if fnv32(data[: HEADER.size - 4]) != header_hash:
        raise ValueError("pack header checksum mismatch")
    if hashlib.sha256(data[HEADER.size :]).digest() != payload_sha:
        raise ValueError("pack payload checksum mismatch")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--verify", type=Path)
    args = parser.parse_args()
    if args.verify:
        verify(args.verify)
        print(f"{args.verify}: valid")
    else:
        build(args.source, args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
