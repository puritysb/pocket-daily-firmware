# Reader fonts from the companion app — v1

Implemented 2026-10-31. The app manages the fonts on the SD card through
CrossPoint's own font and settings handlers; it ships no fonts. New fonts come
from **Settings › System › Manage Fonts** on the reader (CrossPoint's catalog)
or from a `.cpfont` file the user sends through the verified Pocket transfer to
`/.fonts/<family>/`.

## Advertisement

`GET /api/status` reports `"readerFonts": 1` only in the Same Wi-Fi companion
profile (`Profile::COMPANION`). The private-AP profile keeps its minimal route
set and omits the key. Stock CrossPoint has no key but serves the same routes in
File Transfer; the app treats a non-Pocket reader as capable and reports a 404
as unsupported.

## Routes (companion profile)

| Method | Path | Handler | Notes |
|---|---|---|---|
| `GET` | `/api/fonts` | stock `handleFontList` | Marks the registry dirty first, so files the Pocket upload stream published are listed. |
| `POST` | `/api/fonts/delete` | stock `handleFontDelete` | `409 {"error":"Reader is busy"}` while a file mutation or transfer is active. A missing family returns `{"ok":true}` (stock). |
| `GET` | `/api/settings` | stock `handleGetSettings` | Streamed array (about 9 KB). The app reads only `fontFamily`. |
| `POST` | `/api/settings` | stock `handlePostSettings` | Accepts only `fontFamily` and `fontSize` keys (`400 Only reader font settings` otherwise); `409` while busy. |

Payloads are exactly those in [webserver-endpoints.md](webserver-endpoints.md)
(Settings API, Font Management API). `fontFamily` is an option index: the
built-in faces first, then the SD families in `/api/fonts` order. The routes
carry no reader identity; the app confirms `/api/status` `deviceID` and re-reads
the list immediately before each change.

## Reader behavior

A chosen SD family is used for books (`SdCardFontSystem::ensureAutomaticReaderFontLoaded`).
While a built-in face is chosen, Pocket Daily reads books in the automatic
`PocketSansWorld` face when it is installed. Choosing `PocketSansWorld` itself
keeps the automatic, scaled path. The change applies to the next book opened.

## Evidence

- Host: `LibraryFontSystem.ChosenSdFamilyWinsOverAutomaticWorldFace` (fails
  without the change); full host suite 1150/1150.
- X4, developer build `…fix-honor-chosen-reader-font-2b7598ce-w7ded7a3e`, Same
  Wi-Fi: `readerFonts:1`, list of 9 families, settings 9,061 B, font-only and
  invalid-JSON rejections, free heap 38,192 → 37,724 B with the routes
  registered. Rendering a book in a chosen face was not inspected on the panel.
