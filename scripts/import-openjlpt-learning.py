#!/usr/bin/env python3
"""Build Pocket Daily's cumulative N5-N3 source JSON from pinned OpenJLPT data.

The upstream Japanese/English fields and level assignments remain CC BY-SA 4.0.
Korean adaptations live in a separate checked-in ledger so regeneration never
depends on a translation service or silently changes reviewed study text.
"""

from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
PINNED_OPENJLPT_REVISION = "c42fd9fa3777bfc1775446f7c418d549dfd6e4cf"
DEFAULT_TRANSLATIONS = ROOT / "learning/content/jp-n3-ko-translations.json"
DEFAULT_OUTPUT = ROOT / "learning/content/jp-n3-ko.json"
LEVELS = (("n5", 1), ("n4", 2), ("n3", 3))


def load_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def utf8_len(value: str) -> int:
    return len(value.encode("utf-8"))


def compact(values: list[str], limit: int) -> str:
    selected: list[str] = []
    for value in values:
        candidate = "; ".join([*selected, value])
        if utf8_len(candidate) >= limit:
            break
        selected.append(value)
    return "; ".join(selected) or values[0]


def clean_reading(value: str) -> str:
    return value.replace(".", "").replace("-", "")


def is_japanese_reading(value: str) -> bool:
    """Reject mojibake while allowing kana readings and their separators."""
    return bool(value) and all(
        char in " /・ー（）()" or 0x3040 <= ord(char) <= 0x30FF for char in value
    )


def candidate_score(candidate: dict[str, Any], level: int, glyph: str) -> tuple[int, ...]:
    examples = candidate.get("examples") or []
    usable_example = any(
        isinstance(example, dict)
        and glyph in str(example.get("ja", ""))
        and utf8_len(str(example.get("ja", ""))) < 192
        for example in examples
    )
    word = str(candidate.get("word", ""))
    return (
        0 if usable_example else 1,
        0 if int(candidate["levelNum"]) == level else 1,
        0 if word.startswith(glyph) else 1,
        len(word),
        word,
    )


def select_word(
    vocabulary: list[dict[str, Any]],
    glyph: str,
    level: int,
    preferred_word: str,
    preferred_example: str,
) -> dict[str, Any]:
    candidates = [
        row
        for row in vocabulary
        if glyph in str(row.get("word", ""))
        and str(row.get("reading", ""))
        and utf8_len(str(row.get("word", ""))) < 48
        and utf8_len(str(row.get("reading", ""))) < 64
    ]
    if not candidates:
        return {
            "word": glyph,
            "reading": "",
            "meanings": ["kanji character"],
            "examples": [{"ja": f"{glyph}という漢字を覚える。"}],
            "levelNum": level,
        }
    pinned = [
        row
        for row in candidates
        if str(row["word"]) == preferred_word
        and any(
            isinstance(example, dict)
            and str(example.get("ja", "")) == preferred_example
            for example in row.get("examples") or []
        )
    ]
    if pinned:
        return pinned[0]
    candidates.sort(key=lambda row: candidate_score(row, level, glyph))
    return candidates[0]


def select_example(word: dict[str, Any], glyph: str) -> str:
    examples = word.get("examples") or []
    choices = [
        str(example.get("ja", ""))
        for example in examples
        if isinstance(example, dict)
        and glyph in str(example.get("ja", ""))
        and 0 < utf8_len(str(example.get("ja", ""))) < 192
    ]
    if not choices:
        return f"{word['word']}を使った文を読む。"
    return min(choices, key=lambda value: (len(value), value))


def verify_revision(openjlpt: Path) -> None:
    revision = subprocess.check_output(
        ["git", "-C", str(openjlpt), "rev-parse", "HEAD"], text=True
    ).strip()
    if revision != PINNED_OPENJLPT_REVISION:
        raise ValueError(
            f"OpenJLPT checkout must be {PINNED_OPENJLPT_REVISION}, got {revision}"
        )


def build(openjlpt: Path, translations_path: Path, output: Path) -> None:
    verify_revision(openjlpt)
    translations = load_json(translations_path)
    if not isinstance(translations, dict):
        raise ValueError("translation ledger must be an object keyed by kanji")

    kanji: list[dict[str, Any]] = []
    vocabulary: list[dict[str, Any]] = []
    data = openjlpt / "data/json"
    for level_name, level_number in LEVELS:
        kanji.extend(
            {**row, "levelNum": level_number}
            for row in load_json(data / "kanji" / f"{level_name}.json")
        )
        vocabulary.extend(
            {**row, "levelNum": level_number}
            for row in load_json(data / "vocab" / f"{level_name}.json")
        )

    records: list[dict[str, Any]] = []
    for entry in kanji:
        glyph = str(entry["character"])
        level = int(entry["levelNum"])
        korean = translations.get(glyph)
        if not isinstance(korean, dict):
            raise ValueError(f"missing Korean translation for {glyph}")
        word = select_word(
            vocabulary,
            glyph,
            level,
            str(korean.get("word", "")),
            str(korean.get("example", "")),
        )
        # Upstream vocabulary occasionally contains malformed reading text.
        # Keep the pinned source reproducible while allowing the reviewed
        # adaptation ledger to correct that field explicitly.
        reading = str(korean.get("wordReading") or word.get("reading", ""))
        if not reading:
            readings = entry.get("kunyomi") or entry.get("onyomi") or []
            reading = clean_reading(str(readings[0])) if readings else glyph
        if not is_japanese_reading(reading):
            raise ValueError(
                f"invalid Japanese word reading for {glyph}: {reading!r}; "
                "add a reviewed wordReading override to the translation ledger"
            )
        example = str(korean.get("example", ""))
        if glyph not in example or not 0 < utf8_len(example) < 192:
            raise ValueError(f"translation ledger example is invalid for {glyph}")
        if str(korean.get("word", "")) != str(word["word"]):
            raise ValueError(
                f"translation ledger word drift for {glyph}: "
                f"{korean.get('word')!r} != {word['word']!r}"
            )
        meanings = [str(value) for value in entry.get("meanings") or [] if str(value)]
        if not meanings:
            meanings = ["kanji character"]
        records.append(
            {
                "glyph": glyph,
                "level": level,
                "on": "、".join(str(value) for value in entry.get("onyomi") or [])[:20],
                "kun": "、".join(str(value) for value in entry.get("kunyomi") or [])[:20],
                "meaningKo": str(korean["meaningKo"]),
                "meaningEn": compact(meanings, 96),
                "primaryWord": str(word["word"]),
                "wordReading": reading,
                "wordMeaningKo": str(korean["wordMeaningKo"]),
                "example": example,
                "exampleMeaningKo": str(korean["exampleMeaningKo"]),
            }
        )

    expected = 79 + 166 + 367
    if len(records) != expected or len({row["glyph"] for row in records}) != expected:
        raise ValueError(f"expected {expected} unique cumulative N5-N3 kanji, got {len(records)}")

    document = {
        "package": {
            "id": "jp-n3-ko",
            "locale": "ko-KR",
            "title": "Pocket Daily JLPT N3 한자",
            "contentVersion": 3,
            "licenseSpdx": "CC-BY-SA-4.0",
            "sourceRevision": f"openjlpt-{PINNED_OPENJLPT_REVISION[:12]}",
            "attribution": (
                "OpenJLPT/EDRDG/Waller/Tatoeba 기반; Pocket Daily 한국어 각색; "
                "CC BY-SA 4.0"
            ),
            "sources": [
                {
                    "name": "OpenJLPT cumulative N5-N3 dataset",
                    "url": "https://github.com/evanclan/OpenJLPT",
                    "revision": PINNED_OPENJLPT_REVISION,
                    "licenseSpdx": "CC-BY-SA-4.0",
                    "attribution": (
                        "OpenJLPT; upstream EDRDG KANJIDIC2/JMdict, "
                        "Jonathan Waller JLPT resources, and Tatoeba"
                    ),
                },
                {
                    "name": "Pocket Daily Korean adaptations",
                    "url": "https://github.com/puritysb/pocket-daily-reader",
                    "revision": "pocket-ko-2026-08-29",
                    "licenseSpdx": "CC-BY-SA-4.0",
                    "attribution": "Korean meanings and translations by Pocket Daily contributors",
                },
            ],
        },
        "records": records,
    }
    output.write_text(json.dumps(document, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"{output}: {len(records)} cumulative N5-N3 records")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--openjlpt", type=Path, required=True)
    parser.add_argument("--translations", type=Path, default=DEFAULT_TRANSLATIONS)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()
    build(args.openjlpt.resolve(), args.translations, args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
