# Pocket Daily reading shelf v1

The app chooses the order of up to four books from the reader's recent books.
The reader advertises `readingShelf: 1` in its Sync status. This setting is
separate from the Home profile's shelf size and screen layout.

`GET /api/pocket/v1/reading-shelf?deviceID=...` returns the saved selection;
missing or unreadable active and backup records return automatic selection.
`POST` at the same route validates and atomically replaces it, returning 204.
Both routes enforce the existing identity, busy and memory admission checks.

```json
{"schema":1,"automatic":false,"paths":["/Books/Third.epub","/First.epub"]}
```

The root accepts exactly these three keys. Manual mode requires one to four
unique paths in display order. Automatic mode requires an empty path list and
uses recent-book order. Paths are absolute, valid UTF-8 without controls, at
most 255 bytes each. Empty segments, backslashes and dot traversal are rejected.
The JSON body is limited to 8192 bytes. POST also requires every selected path
to remain in the recent-book list and exist on SD; otherwise it returns 409
so the app can refresh that list. Malformed requests return 400; allocation
failures return 503; storage failures return 500.

The SD record `/.crosspoint/pocket-reading-shelf.bin` uses `.tmp` and `.bak`
companions. PDSH version 1 is exactly 1036 bytes: magic(4), version(1),
automatic flag(1), count(1), reserved zero(1), four zero-padded 256-byte path
slots, and little-endian CRC32 over the preceding bytes. Unused slots are zero.
Writes verify temporary and active bytes; an invalid active record cannot
replace the valid recovery backup. An unchanged selection avoids another write.

The Home shelf resolves selected paths against current recent-book metadata,
uses each book's actual cover when available, and opens the chosen book on
Confirm. PDLI5 carries the resolved recent books and cover descriptors so the
app preview and device share rendering. Tests cover ordered, nonuniform paths,
strict parsing, CRC rejection and failed publication; physical SD durability,
button interaction and heap headroom require separate hardware checks.
