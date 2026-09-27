#!/usr/bin/env python3
"""
Generate an EPUB of many short spine items (one to three paragraphs each), like an
aphorism collection where nearly every page turn enters a new chapter. Used by
test/reader_layout to model the reader's layout-ahead cadence.

    python3 scripts/generate_short_spines_test_epub.py   # -> test/epubs/short-spines.epub

Stdlib only; deterministic (fixed seed, fixed ZIP timestamps).
"""

import random
import zipfile
from pathlib import Path

OUTPUT = Path(__file__).parent.parent / "test" / "epubs" / "short-spines.epub"
SPINES = 12
WORDS = (
    "der mensch ist etwas das ueberwunden werden soll was habt ihr getan ihn zu ueberwinden "
    "alle wesen bisher schufen etwas ueber sich hinaus und ihr wollt die ebbe dieser grossen flut sein"
).split()


def write(z, name, data, stored=False):
    info = zipfile.ZipInfo(name, date_time=(2026, 9, 28, 0, 0, 0))
    info.compress_type = zipfile.ZIP_STORED if stored else zipfile.ZIP_DEFLATED
    z.writestr(info, data)


def main():
    rng = random.Random(7)
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(OUTPUT, "w") as z:
        write(z, "mimetype", "application/epub+zip", stored=True)
        write(
            z,
            "META-INF/container.xml",
            '<?xml version="1.0"?><container version="1.0" '
            'xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles>'
            '<rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>'
            "</rootfiles></container>",
        )
        manifest = "".join(
            f'<item id="c{i}" href="c{i}.xhtml" media-type="application/xhtml+xml"/>' for i in range(SPINES)
        )
        spine = "".join(f'<itemref idref="c{i}"/>' for i in range(SPINES))
        write(
            z,
            "OEBPS/content.opf",
            '<?xml version="1.0"?><package xmlns="http://www.idpf.org/2007/opf" version="3.0" '
            'unique-identifier="id"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/">'
            '<dc:identifier id="id">short-spines</dc:identifier><dc:title>Short spines</dc:title>'
            "<dc:language>de</dc:language></metadata><manifest>"
            '<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>'
            f"{manifest}</manifest><spine>{spine}</spine></package>",
        )
        nav = "".join(f'<li><a href="c{i}.xhtml">{i + 1}</a></li>' for i in range(SPINES))
        write(
            z,
            "OEBPS/nav.xhtml",
            '<?xml version="1.0"?><html xmlns="http://www.w3.org/1999/xhtml" '
            'xmlns:epub="http://www.idpf.org/2007/ops"><body><nav epub:type="toc"><ol>'
            f"{nav}</ol></nav></body></html>",
        )
        for i in range(SPINES):
            paragraphs = "".join(
                "<p>" + " ".join(rng.choice(WORDS) for _ in range(rng.randint(20, 60))) + ".</p>"
                for _ in range(rng.randint(1, 3))
            )
            write(
                z,
                f"OEBPS/c{i}.xhtml",
                '<?xml version="1.0"?><html xmlns="http://www.w3.org/1999/xhtml"><head>'
                f"<title>{i + 1}</title></head><body><h3>{i + 1}.</h3>{paragraphs}</body></html>",
            )


if __name__ == "__main__":
    main()
