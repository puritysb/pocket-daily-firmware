# Article library v1

2026-09-27. Companion-owned design and wire layout:
[Articles](../../pocket-daily/docs/ARTICLES.md).

Firmware owns Pocket Daily → Articles (the Left front button on Pocket Daily's
Home; the stock CrossPoint Home menu gains only the Pocket Daily entry),
bounded EPUB metadata scanning, existing EPUB
reader integration, read markers and explicit reader-copy deletion. The app owns
browser sharing, page extraction, local source retention and explicit transfers.
No reader-side web client, RSS fetcher, background sync or new network endpoint.

The status capability is `articleLibrary: 1`. The EPUB and its list metadata are
published together through the existing verified stream/commit transport.
Entry filename, binary metadata offsets/CRC, limits and acceptance gates are
specified once in the linked contract. `src/articles/ArticleFormat.h` holds the
portable identity, UTF-8 and metadata checks; host tests mirror the Swift writer.
`ArticleStorage.h` reads only the first two stored ZIP local entries and validates
metadata before list display. No lib/Epub cache format is changed.

The newest 128 valid files are indexed with at most eight visible metadata rows;
older entries remain on SD and become visible when newer ones are removed. The
activity state is under 13 KiB, allocated with makeUniqueNoThrow on enter and
freed on exit. Runtime free heap/largest block have not been measured.

Completion marker writes happen once per entry into the end-of-book state.
Articles stay in their library regardless of move-finished-books settings.
Confirmed deletion removes the selected file before clearing caches/recents;
failed file deletion leaves the file in the list and shows a retry instruction.
Reconnection never initiates a companion send. An explicit replacement uses the
same UUID path, clears reader cache and removes the completion marker.

Physical X3/X4 reading, sharing from Safari, four orientations, SD errors and
heap remain acceptance gates. Builds/host tests are not hardware evidence.
No installation, commit, push or release is part of this change.
