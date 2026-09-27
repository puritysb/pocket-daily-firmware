#!/usr/bin/env python3
"""Rebuild the reading-progress cross-check EPUB fixtures.

The XPointer fixtures (app-xpointers-*.json) were produced by the Pocket Daily
app's reader engine (Support/ReaderEngine/xpointer.js) against the original
books. XPointers depend only on the XHTML text, so the committed EPUBs keep
every spine document byte-for-byte and only shrink the rest:

- pocket-daily-epub-check.epub: original app-generated sample (Korean, emoji,
  one 30k-character paragraph), re-zipped with deflate.
- frankenstein-se.epub: Standard Ebooks "Frankenstein" (public domain text,
  CC0 markup, https://standardebooks.org), images replaced by 1x1 placeholders.

Usage: make_fixtures.py <pocket-daily-epub-check.epub> <frankenstein-se.epub>
"""
import sys
import zipfile
from pathlib import Path

PNG_1X1 = bytes.fromhex(
    "89504e470d0a1a0a0000000d4948445200000001000000010806000000"
    "1f15c4890000000d49444154789c6360000002000154a24f5d0000000049454e44ae426082")
JPEG_1X1 = bytes.fromhex(
    "ffd8ffe000104a46494600010100000100010000ffdb004300080606070605080707070909080a0c"
    "140d0c0b0b0c1912130f141d1a1f1e1d1a1c1c20242e2720222c231c1c2837292c30313434341f27"
    "393d38323c2e333432ffc0000b080001000101011100ffc4001f00000105010101010101000000000000"
    "00000102030405060708090a0bffc400b5100002010303020403050504040000017d01020300041105"
    "122131410613516107227114328191a1082342b1c11552d1f02433627282090a161718191a25262728"
    "292a3435363738393a434445464748494a535455565758595a636465666768696a737475767778797a"
    "838485868788898a92939495969798999aa2a3a4a5a6a7a8a9aab2b3b4b5b6b7b8b9bac2c3c4c5c6c7"
    "c8c9cad2d3d4d5d6d7d8d9dae1e2e3e4e5e6e7e8e9eaf1f2f3f4f5f6f7f8f9faffda0008010100003f00"
    "fbd3ffd9")


def rezip(source: Path, target: Path, placeholders: bool) -> None:
    with zipfile.ZipFile(source) as src, zipfile.ZipFile(target, "w") as dst:
        for info in src.infolist():
            data = src.read(info.filename)
            lower = info.filename.lower()
            if placeholders and lower.endswith(".png"):
                data = PNG_1X1
            elif placeholders and (lower.endswith(".jpg") or lower.endswith(".jpeg")):
                data = JPEG_1X1
            method = zipfile.ZIP_STORED if info.filename == "mimetype" else zipfile.ZIP_DEFLATED
            dst.writestr(zipfile.ZipInfo(info.filename, date_time=(1980, 1, 1, 0, 0, 0)), data,
                         compress_type=method, compresslevel=9)


if __name__ == "__main__":
    here = Path(__file__).resolve().parent
    rezip(Path(sys.argv[1]), here / "pocket-daily-epub-check.epub", False)
    rezip(Path(sys.argv[2]), here / "frankenstein-se.epub", True)
