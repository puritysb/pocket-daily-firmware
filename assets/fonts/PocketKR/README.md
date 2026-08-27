# PocketSansKR

`PocketSansKR_12.cpfont` is Pocket Daily's SD-distributed Korean UI family.
It contains regular (500) and bold (700) Noto Sans KR instances at 12 px with
printable ASCII, Korean reading punctuation, Jamo, compatibility Jamo, and all
modern Hangul syllables.

The font is intentionally not embedded in the firmware: its broad Hangul
coverage is several megabytes and would overflow the ESP32-C3 application
partition. Copy the directory to `/.fonts/PocketSansKR/` on the SD card. The
legacy `AgentDeckKR` family remains a lower-priority fallback during migration.

Rebuild both Pocket font families with:

```sh
python3 -m pip install fonttools freetype-py pillow
python3 scripts/build-pocket-fonts.py
```

The source is Google Fonts' Noto Sans KR variable font pinned to commit
`ec626514f79f831f1ab848a82114a0ce7e2d6372`, SHA-256
`194018e6b2b293a7964f037b25c0249ce1418bc9ab3c971060a03aa57861e252`.
The output is licensed under SIL Open Font License 1.1; see `OFL.txt`.
