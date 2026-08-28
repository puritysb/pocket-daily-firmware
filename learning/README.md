# Pocket Daily learning packs

Pocket Daily reads its Japanese course from an SD-backed `.pdl` pack. The
active Korean N3-oriented pack lives at:

```text
/pocket-daily/learning/jp-n3-ko.pdl
```

Build the repository-authored starter pack:

```sh
python scripts/build-learning-pack.py
```

The build also writes `jp-n3-ko.manifest.json`. A Surface provider can copy its
`learningPack` object directly into `card_feed` and serve the adjacent `.pdl`
file from the endpoint described below.

Copy `learning/dist/jp-n3-ko.pdl` to the path above on the SD card. The device
validates the fixed-format header, mandatory SPDX licence and attribution,
header checksum, file size, and complete payload SHA-256 before reading it.

## Licence policy

Every source file must declare `licenseSpdx`, `sourceRevision`, and
`attribution`. The builder refuses content without these fields. The initial
Korean lessons in this repository are original Pocket Daily contributor
content licensed under CC BY-SA 4.0; see `LICENSE-CC-BY-SA-4.0.txt`.

Third-party imports must remain reproducible and pinned. KANJIDIC2/JMdict
derivatives must retain EDRDG attribution and CC BY-SA 4.0. Do not copy JLPT
official practice questions or proprietary N3 lists into the pack. JLPT does
not publish a current official level-by-level vocabulary/kanji specification;
course-level tags therefore need to be described as N3-oriented curation.

## Wireless update contract

A compatible Surface provider may add this object to any full or unchanged
`card_feed` response:

```json
{
  "learningPack": {
    "id": "jp-n3-ko",
    "version": 2,
    "format": 1,
    "size": 123456,
    "md5": "0123456789abcdef0123456789abcdef",
    "licenseSpdx": "CC-BY-SA-4.0"
  }
}
```

On an explicit Pocket Daily Sync, the device downloads the advertised bytes
from:

```text
GET /learning/pack?id=jp-n3-ko&version=2
```

Surface identity headers and the existing `token` query are included. The
download lands in a temporary SD file, is checked against the advert and its
internal pack metadata, then atomically replaces the active pack. Automatic
wake pulls only learn about the advert; they do not download large study data.
