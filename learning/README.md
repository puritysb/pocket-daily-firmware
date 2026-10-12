# Pocket Daily learning packs

The daily vocabulary course is optional and disabled on a fresh profile.
Enable Daily word deliberately in the companion’s reader layout; the normal
Home and sleep defaults show the current book or an empty Library slot.
Pocket Daily reads the selected Japanese course from an SD-backed `.pdl` pack. The
active Korean N3-oriented pack lives at:

```text
/pocket-daily/learning/jp-n3-ko.pdl
```

The current content version contains the cumulative N5–N3 foundation: 79 N5,
166 N4, and 367 N3-oriented kanji, for 612 unique lessons. Level assignment is
unofficial because the JLPT organization does not publish a current item list.
See [`NOTICE.md`](NOTICE.md) for the pinned OpenJLPT/EDRDG/Waller/Tatoeba
attribution and modification ledger.

Recreate the normalized source from the pinned OpenJLPT checkout and the
checked-in Korean translation ledger, then build the pack:

```sh
python scripts/import-openjlpt-learning.py --openjlpt /path/to/OpenJLPT
python scripts/build-learning-pack.py
```

The build also writes `jp-n3-ko.manifest.json` for reproducible packaging.
The current companion has no learning-pack catalog or installer.

Copy `learning/dist/jp-n3-ko.pdl` to the path above on the SD card. The device
validates the fixed-format header, mandatory SPDX licence and attribution,
header checksum, file size, and complete payload SHA-256 before reading it.

## Licence policy

Every source file must declare `licenseSpdx`, `sourceRevision`, and
`attribution`. The builder refuses content without these fields. The adapted
Korean course is distributed under CC BY-SA 4.0; see
`LICENSE-CC-BY-SA-4.0.txt` and `NOTICE.md`.

Third-party imports must remain reproducible and pinned. KANJIDIC2/JMdict
derivatives must retain EDRDG attribution and CC BY-SA 4.0. Do not copy JLPT
official practice questions or proprietary N3 lists into the pack. JLPT does
not publish a current official level-by-level vocabulary/kanji specification;
course-level tags therefore need to be described as N3-oriented curation.

## Historical provider contract

The former Surface/AgentDeck provider and its `/learning/pack` endpoint are
retired. They are not prerequisites for offline learning and are not a current
companion installation path. Install the validated `.pdl` on SD explicitly.
A future app pack installer must preserve preview, license, version and an
explicit user choice; it must not populate a fresh reader with assumed interests.
