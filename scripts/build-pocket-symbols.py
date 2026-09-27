#!/usr/bin/env python3
"""Rebuild PocketSymbols_12.cpfont, the reader's glyph-fallback font.

The firmware borrows a glyph from this font when the reading font lacks it
(emoji, dingbats, arrows, math and technical symbols). Sources are pinned by
Google Fonts commit and SHA-256, all SIL Open Font License 1.1 without a
Reserved Font Name; the derived family is still renamed to PocketSymbols.

Run with a Python environment containing lib/EpdFont/scripts/requirements.txt:

    python scripts/build-pocket-symbols.py
"""

from __future__ import annotations

import hashlib
import shutil
import subprocess
import sys
import tempfile
import unicodedata
import urllib.request
from pathlib import Path

from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont


ROOT = Path(__file__).resolve().parents[1]
GOOGLE_FONTS_COMMIT = "23e54b51ddffbc7713c583748e3bd86f62b1fa4a"
RAW = f"https://raw.githubusercontent.com/google/fonts/{GOOGLE_FONTS_COMMIT}/ofl"
CACHE = ROOT / "lib/EpdFont/scripts/downloaded_fonts/PocketSymbols"
OUTPUT_DIR = ROOT / "assets/fonts/PocketSymbols"
OUTPUT = OUTPUT_DIR / "PocketSymbols_12.cpfont"
COVERAGE = OUTPUT_DIR / "coverage.txt"
CONVERTER = ROOT / "lib/EpdFont/scripts/fontconvert_sdcard.py"
CHECKER = ROOT / "scripts/check-cpfont-coverage.py"
EMOJI_SCALE = 1.2

# (local name, URL path, SHA-256). Order is lookup priority: monochrome emoji
# first, then symbols, then math.
SOURCES = [
    ("NotoEmoji-wght.ttf", "notoemoji/NotoEmoji%5Bwght%5D.ttf",
     "de6c18832938afc99caf132b39d6a30a19bac7f2e812e28db2535b4608d27551"),
    ("NotoSansSymbols2-Regular.ttf", "notosanssymbols2/NotoSansSymbols2-Regular.ttf",
     "7d5fb73b7ca67a6798101741f5d280a3d016a56a197afcd4199dbb57b4b82a21"),
    ("NotoSansMath-Regular.ttf", "notosansmath/NotoSansMath-Regular.ttf",
     "3f495fe933c06786e4d5f6d86b8ee70b6753a68ee3b9d87528726de0f6e2c47d"),
]
LICENSES = [
    ("NotoEmoji-OFL.txt", "notoemoji/OFL.txt"),
    ("NotoSansSymbols2-OFL.txt", "notosanssymbols2/OFL.txt"),
    ("NotoSansMath-OFL.txt", "notosansmath/OFL.txt"),
]

# Unicode blocks the fallback serves. Latin, CJK and the other scripts belong to
# the reading fonts; Braille and mathematical alphanumerics are left out to keep
# the file and its resident interval table small.
BLOCKS = [
    (0x00A9, 0x00A9), (0x00AE, 0x00AE),  # (c) (R) emoji forms
    (0x203C, 0x203C), (0x2049, 0x2049),  # double exclamation, !?
    (0x2100, 0x214F),  # Letterlike Symbols
    (0x2190, 0x21FF),  # Arrows
    (0x2200, 0x22FF),  # Mathematical Operators
    (0x2300, 0x23FF),  # Miscellaneous Technical
    (0x2460, 0x24FF),  # Enclosed Alphanumerics
    (0x25A0, 0x25FF),  # Geometric Shapes
    (0x2600, 0x26FF),  # Miscellaneous Symbols
    (0x2700, 0x27BF),  # Dingbats
    (0x27C0, 0x27FF),  # Misc Mathematical Symbols-A, Supplemental Arrows-A
    (0x2900, 0x2AFF),  # Supplemental Arrows-B, Misc Math-B, Supplemental Math Operators
    (0x2B00, 0x2BFF),  # Miscellaneous Symbols and Arrows
    (0x3030, 0x3030), (0x303D, 0x303D), (0x3297, 0x3297), (0x3299, 0x3299),
    (0x1F000, 0x1F0FF),  # Mahjong, Domino, Playing Cards
    (0x1F100, 0x1F2FF),  # Enclosed Alphanumeric / Ideographic Supplement
    (0x1F300, 0x1F6FF),  # Pictographs, Emoticons, Ornamental Dingbats, Transport
    (0x1F780, 0x1F8FF),  # Geometric Shapes Extended, Supplemental Arrows-C
    (0x1F900, 0x1FAFF),  # Supplemental Symbols and Pictographs, Extended-A
]

# The firmware never draws these (zero width): Default_Ignorable_Code_Point,
# emoji modifiers. Keep them out of the font.
def invisible(cp: int) -> bool:
    return (
        cp in (0x00AD, 0x034F, 0x061C, 0x3164, 0xFEFF, 0xFFA0)
        or 0x115F <= cp <= 0x1160 or 0x17B4 <= cp <= 0x17B5 or 0x180B <= cp <= 0x180F
        or 0x200B <= cp <= 0x200F or 0x202A <= cp <= 0x202E or 0x2060 <= cp <= 0x206F
        or 0xFE00 <= cp <= 0xFE0F or 0xFFF0 <= cp <= 0xFFF8 or 0x1BCA0 <= cp <= 0x1BCA3
        or 0x1D173 <= cp <= 0x1D17A or 0xE0000 <= cp <= 0xE0FFF or 0x1F3FB <= cp <= 0x1F3FF
    )


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def fetch(url_path: str, destination: Path, expected_sha: str | None) -> Path:
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists() and (expected_sha is None or sha256(destination) == expected_sha):
        return destination
    temporary = destination.with_suffix(destination.suffix + ".download")
    urllib.request.urlretrieve(f"{RAW}/{url_path}", temporary)
    if expected_sha is not None and sha256(temporary) != expected_sha:
        actual = sha256(temporary)
        temporary.unlink(missing_ok=True)
        raise RuntimeError(f"{url_path}: checksum mismatch, expected {expected_sha}, got {actual}")
    temporary.replace(destination)
    return destination


def main() -> int:
    sources = [fetch(path, CACHE / name, digest) for name, path, digest in SOURCES]
    for name, path in LICENSES:
        text = fetch(path, CACHE / name, None).read_text(encoding="utf-8")
        if "Reserved Font Name" in text.split("PREAMBLE")[0]:
            raise RuntimeError(f"{name}: a Reserved Font Name appeared; review the derived font name")

    covered: set[int] = set()
    for source in sources:
        font = TTFont(source)
        try:
            covered.update(font.getBestCmap().keys())
        finally:
            font.close()
    wanted = sorted(
        cp for cp in covered
        if any(first <= cp <= last for first, last in BLOCKS) and not invisible(cp)
        and unicodedata.category(chr(cp)) not in ("Cc", "Cf", "Zs")
    )

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    with COVERAGE.open("w", encoding="utf-8") as out:
        out.write("# PocketSymbols coverage, generated by scripts/build-pocket-symbols.py; do not edit.\n")
        for cp in wanted:
            out.write(f"U+{cp:04X}\n")

    with tempfile.TemporaryDirectory(prefix="pocket-symbols-") as directory:
        emoji_medium = Path(directory) / "NotoEmoji-Medium.ttf"
        variable = TTFont(sources[0])
        try:
            static = instantiateVariableFont(variable, {"wght": 500}, updateFontNames=False, optimize=False)
            # Noto Emoji is drawn larger than text (advance 1.25 em). Enlarging
            # the em by EMOJI_SCALE shrinks every outline and advance uniformly
            # so an emoji matches a Hangul syllable or ideograph at the same size.
            head = static["head"]
            head.unitsPerEm = round(head.unitsPerEm * EMOJI_SCALE)
            static.save(emoji_medium)
            static.close()
        finally:
            variable.close()
        intervals = ",".join(f"(0x{first:04X}-0x{last:04X})" for first, last in BLOCKS)
        subprocess.run(
            [
                sys.executable, str(CONVERTER),
                "--intervals", intervals,
                "--coverage-file", str(COVERAGE),
                "--size", "12",
                "--regular", str(emoji_medium),
                "--fallback-regular", str(sources[1]),
                "--fallback-regular", str(sources[2]),
                "--name", "PocketSymbols",
                "-o", str(OUTPUT),
            ],
            check=True,
        )
    for name, _ in LICENSES:
        shutil.copyfile(CACHE / name, OUTPUT_DIR / name)
    subprocess.run([sys.executable, str(CHECKER), str(OUTPUT), str(COVERAGE)], check=True)
    print(f"{OUTPUT.relative_to(ROOT)} {OUTPUT.stat().st_size} bytes sha256={sha256(OUTPUT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
