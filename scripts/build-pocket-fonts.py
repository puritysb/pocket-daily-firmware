#!/usr/bin/env python3
"""Rebuild the small, deterministic Pocket Daily firmware font assets."""

from __future__ import annotations

import hashlib
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path

from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont


ROOT = Path(__file__).resolve().parents[1]
JP_SOURCE_COMMIT = "295d98a7a0c17c68f1341eaeea354e7960ea70d3"
JP_SOURCE_URL = (
    "https://raw.githubusercontent.com/google/fonts/"
    f"{JP_SOURCE_COMMIT}/ofl/notosansjp/NotoSansJP%5Bwght%5D.ttf"
)
JP_SOURCE_SHA256 = "c2f3b4d463500a2ddcd3849cded1fceeb9fd6d1c32e6cbecd568453ba50fc68f"
JP_CACHE = ROOT / "lib/EpdFont/scripts/downloaded_fonts/PocketSansJP/NotoSansJP-wght.ttf"
KR_SOURCE_COMMIT = "ec626514f79f831f1ab848a82114a0ce7e2d6372"
KR_SOURCE_URL = (
    "https://raw.githubusercontent.com/google/fonts/"
    f"{KR_SOURCE_COMMIT}/ofl/notosanskr/NotoSansKR%5Bwght%5D.ttf"
)
KR_SOURCE_SHA256 = "194018e6b2b293a7964f037b25c0249ce1418bc9ab3c971060a03aa57861e252"
KR_CACHE = ROOT / "lib/EpdFont/scripts/downloaded_fonts/PocketSansKR/NotoSansKR-wght.ttf"
CONVERTER = ROOT / "lib/EpdFont/scripts/fontconvert_sdcard.py"
JP_COVERAGE = ROOT / "assets/fonts/PocketJP/coverage.txt"
JP_OUTPUT = ROOT / "assets/fonts/PocketJP/PocketSansJP_12.cpfont"
KR_COVERAGE = ROOT / "assets/fonts/PocketKR/coverage.txt"
KR_OUTPUT = ROOT / "assets/fonts/PocketKR/PocketSansKR_12.cpfont"
FALLBACK_DIR = ROOT / "lib/EpdFont/builtinFonts/source/NotoSans"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def source_font(url: str, expected_sha: str, cache: Path) -> Path:
    cache.parent.mkdir(parents=True, exist_ok=True)
    if cache.exists() and sha256(cache) == expected_sha:
        return cache
    temporary = cache.with_suffix(".download")
    urllib.request.urlretrieve(url, temporary)
    actual = sha256(temporary)
    if actual != expected_sha:
        temporary.unlink(missing_ok=True)
        raise RuntimeError(f"source checksum mismatch: expected {expected_sha}, got {actual}")
    temporary.replace(cache)
    return cache


def instance(source: Path, weight: int, destination: Path) -> None:
    variable = TTFont(source)
    try:
        static = instantiateVariableFont(
            variable, {"wght": weight}, updateFontNames=True, optimize=False
        )
        try:
            static.save(destination)
        finally:
            static.close()
    finally:
        variable.close()


def main() -> int:
    jp_source = source_font(JP_SOURCE_URL, JP_SOURCE_SHA256, JP_CACHE)
    kr_source = source_font(KR_SOURCE_URL, KR_SOURCE_SHA256, KR_CACHE)
    with tempfile.TemporaryDirectory(prefix="pocket-font-") as directory:
        temporary = Path(directory)
        jp_regular = temporary / "PocketSansJP-Medium.ttf"
        jp_bold = temporary / "PocketSansJP-Bold.ttf"
        kr_regular = temporary / "PocketSansKR-Medium.ttf"
        kr_bold = temporary / "PocketSansKR-Bold.ttf"
        instance(jp_source, 500, jp_regular)
        instance(jp_source, 700, jp_bold)
        instance(kr_source, 500, kr_regular)
        instance(kr_source, 700, kr_bold)
        subprocess.run(
            [
                sys.executable,
                str(CONVERTER),
                "--intervals",
                "reading,cjk",
                "--size",
                "12",
                "--regular",
                str(jp_regular),
                "--bold",
                str(jp_bold),
                "--fallback-regular",
                str(FALLBACK_DIR / "NotoSans-Regular.ttf"),
                "--fallback-bold",
                str(FALLBACK_DIR / "NotoSans-Bold.ttf"),
                "--coverage-file",
                str(JP_COVERAGE),
                "--name",
                "PocketSansJP",
                "-o",
                str(JP_OUTPUT),
            ],
            check=True,
        )
        KR_OUTPUT.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(
            [
                sys.executable,
                str(CONVERTER),
                "--intervals",
                "reading,hangul",
                "--size",
                "12",
                "--regular",
                str(kr_regular),
                "--bold",
                str(kr_bold),
                "--fallback-regular",
                str(FALLBACK_DIR / "NotoSans-Regular.ttf"),
                "--fallback-bold",
                str(FALLBACK_DIR / "NotoSans-Bold.ttf"),
                "--name",
                "PocketSansKR",
                "-o",
                str(KR_OUTPUT),
            ],
            check=True,
        )
    checker = ROOT / "scripts/check-cpfont-coverage.py"
    for output, coverage in ((JP_OUTPUT, JP_COVERAGE), (KR_OUTPUT, KR_COVERAGE)):
        subprocess.run([sys.executable, str(checker), str(output), str(coverage)], check=True)
        print(f"{output.relative_to(ROOT)} sha256={sha256(output)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
