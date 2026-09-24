# Pocket deck cache v7

This is the reader's internal offline cache, not a companion upload format.
No Swift HTTP/Bluetooth contract changes. Implemented in
[`deck_store.cpp`](../src/agentdeck/deck_store.cpp), with actual serializer
fault-injection tests in [`DeckStoreTest.cpp`](../test/deck_store/DeckStoreTest.cpp).

## Layout

Two private files: `/.crosspoint/pocket-daily-deck.0.bin` and `.1.bin`.
Each contains, in order:

1. Existing 12-byte header: uint32 magic `0x314B4441`, uint8 version **7**,
   uint8 legacy record count (0–10), uint16 `sizeof(Record)`, uint32 saved epoch.
2. uint32 generation, starting at 1; never wrap `UINT32_MAX`.
3. Existing v6 payload: 12-byte feed signature, 6-byte server HH:MM, `Glance`,
   uint8 card count (0–3), fixed three-`Card` array, then `count` legacy `Record`s.
4. uint32 CRC-32/ISO-HDLC over every preceding byte (reflected polynomial
   `0xEDB88320`, initial and final XOR `0xFFFFFFFF`).

As in v6, scalar byte order and model layouts are the ESP32 little-endian ABI;
this is not a portable interchange format. Model layout changes must bump the
version. File length must match exactly; extra bytes are invalid.

## Publication and recovery

- Inspect both slots using a 128-byte scratch buffer, with no second snapshot.
- Select the highest valid generation (equal generations prefer slot 0).
- On save, truncate/write only the other slot; the selected slot is untouched.
  Refuse a save if either existing slot cannot be read, rather than guessing
  that an unreadable slot is disposable. Refuse generation exhaustion.
- Closing the file is followed by length/header/generation/CRC readback,
  including comparison to the intended CRC. No delete or rename is involved.
- Boot selects the highest valid slot. Actual loaded bytes are CRC checked
  again; failed reads fall back to the other valid slot. An empty deck is a
  valid revision and does not fall back merely because it contains no cards.
- If no v7 slot loads, try v6 `pocket-daily-deck.bin`, then
  `agentdeck-deck.bin`. These old files are never rewritten or removed by v7.
  An old firmware downgrade reads that older v6 snapshot, not v7 changes.
- Calls are serialized by the activity loop. The HAL still serializes each
  underlying SD operation with font/render SD reads.

## Limits

This preserves a previous file when a candidate write is interrupted or
corrupted. It does **not** make FAT directory metadata, the SD controller,
cached acknowledgments, or the whole volume power-fail-safe. Hardware power-cut
validation remains outstanding. CRC detects accidental corruption, not forgery.

If writing completes but readback fails, `save` returns false even though a valid
new generation may already exist. A subsequent load resolves that ambiguity;
failure is not proof that the old generation is still selected. Both copies
remain available. When both v7 slots fail, falling back to v6 can show stale
content. No success is reported from a truncated/unchecked v7 snapshot.

This cache repair is not the planned manifest-based companion content-revision
transaction. Content editing, app activation receipts and multi-file rollback
still require that separate cross-repository implementation.
