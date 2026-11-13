# Pocket Daily dated agenda v1

The app composes local calendar dates and sends a bounded window to a compatible
reader. There is no cloud service or EventKit implementation on the reader.
`/api/status` advertises `agenda: 1` on the Sync profile.

`GET /api/pocket/v1/agenda?deviceID=...` reads the current document. Missing or
unreadable active and backup records return the empty document. `POST` at the
same route atomically replaces it and returns 204 after verified storage.
Both operations use the existing identity, busy and memory admission checks.
No reader display or profile is changed by these requests.

```json
{"schema":1,"days":[{"date":"2028-02-29","events":[{"startHm":"09:30","endHm":"10:00","title":"Reading group"}],"omittedEventCount":0}]}
```

The root and nested objects reject unknown or missing keys. Documents are at
most 8192 bytes, containing zero to seven strictly increasing, valid Gregorian
dates in 1970–9999. Each date contains zero to three events. Titles are nonempty
strict UTF-8 without controls, at most 48 bytes. Start/end times are both empty
for all-day events, or both `HH:MM` with end at or after start. Multi-day events
are split and clipped into local dates by the app. `omittedEventCount` is an
integer from 0 to 65535. A date with no events is explicitly synchronized;
an absent date has not been downloaded. `days: []` clears the saved agenda.

The SD record is `/.crosspoint/pocket-agenda.bin`, with `.tmp` and `.bak`
publication/recovery companions. PDAG version 1 occupies exactly 1343 bytes:
magic(4), version(1), reserved zero(1), payload length LE16(1331), day count(1),
seven fixed slots, then CRC32 of all preceding bytes. Each slot has date LE32
(YYYYMMDD), omitted count LE16, event count u8, and three events (6-byte start,
6-byte end, 49-byte title). Text is NUL-terminated and zero-filled; unused
slots are canonical zeros. No C++ padding is serialized.

Writes verify temporary bytes, preserve the previous record through rename,
and verify the active bytes before deleting the backup. Failed validation,
short writes and failed publication do not replace the previous valid record.
The old PDGL today/weather file and schema remain unchanged.

On the Calendar Home page, Confirm enters date selection. Page buttons move
one Gregorian day and cross month/year boundaries. Confirm or Home leaves
selection and returns to the current date. Event dots mark downloaded dates
with events or omitted items. The detail area displays the selected date and
its events, `+N` for omitted or space-limited entries, and a dash for an absent
date. Landscape uses separate grid/detail columns. Selection is transient and
does not write the SD card on every button press.

The shared renderer receives `HomeView.agenda` and `calendarSelection`.
PDLI schema 5 carries the validated agenda after recent-cover descriptors;
older snapshots retain their prior semantics. The companion and device use
the same renderer. Host tests establish parser, storage and date arithmetic
behavior, not physical SD durability, buttons, e-paper quality or heap limits.
