# Bounded File Transfer downloads

The legacy File Transfer `GET /download?path=…` is independent of the
companion's `/api/pocket/v1/files/content` protocol. The R11 change preserves
HTTP/1.0 and HTTP/1.1, MIME, attachment filename, CORS and exact Content-Length.
It changes transport flow control, not the shipping firmware confirmation flow.

`CrossPointWebServer.cpp` uses HAL storage, a snapshot length and the existing
4 KiB main-task firmware staging scratch. Upload-stream reception owns that
scratch exclusively: `handleClient()` returns while `stream.receiving`, and
no callback/event pumping occurs inside the download borrow. There is no new
permanent buffer or per-packet allocation. Separate header frames stay below
256 B on the actual C3 compiler (measured maximum 192 B, developer/production).

The pinned SDK NetworkClient write path can wait in one-second select cycles;
its clear discards RX and flush does not drain TX. The bounded writer instead
waits up to 20 ms for socket writability, sends nonblocking chunks at the
configured 1436-byte MSS, accounts for short sends and retries backpressure
within the same response while yielding/feeding the watchdog. TCP_NODELAY is
set. A 5-second idle deadline and size-based total budget (30 seconds plus
one second per rounded-up 16 KiB, capped at one hour) are independent.
SO_SNDTIMEO is explicitly one second to bound the SDK/lwIP close path when
low-memory FIN allocation must wait. These budgets cannot bound a blocked HAL
read itself. A zero/short SD read fails the transfer; it cannot certify a
truncated body against the declared length. The connection is explicitly closed.

The [lwIP send low-water documentation](https://www.nongnu.org/lwip/2_0_x/group__lwip__opts__tcp.html)
explains writability. Espressif's
[close implementation](https://github.com/espressif/esp-lwip/blob/2.2.0-esp/src/api/api_msg.c)
uses the configured send timeout in its FIN-allocation wait. Socket acceptance
is not proof of peer receipt: hardware checks compare the complete received
bytes/hash. The development-only `devDownload` counters contain no path,
identity or payload and are compiled out of gh_release.

## R11 evidence and limits — 2026-10-28

Private ignored receipts: `build/file-transfer-fix-20261028`.
Final developer image `wc5c8af2c`, SHA
`ab6b24aec4d930d9ba2ba493f2749aee2e3108f1e4624a71a8c3ee6ce8d31f78`,
6,483,296 B. Final gh_release0.1.0 SHA
`eb403cf1682f924d985fb8758a31ebc368631be998cc250e4293e17d4a3e4b84`,
6,371,264 B. Hardware observations below are developer-image evidence; the
production BIN was built, not installed for File Transfer acceptance.

- Baseline ordinary downloads passed18/18 on each reader; the historical
  intermittent failure did not recur in that sample. Early raw-send prototypes
  did fail on X4; those failures remain recorded. A MSS-only change also failed.
  The successful final change combines writable waiting, bounded flow control
  and explicit closure; no single SDK mechanism was experimentally isolated.
- X3 final ordinary18/18 and X4 immutable ordinary15/15 passed without request
  retries, including three whole6.48 MB update.bin downloads per reader.
- X4 log assertions initially refused3 responses before reading the body because
  the expected length was stale. A separate full5,548,577 B log download retained
  the initial5,548,475 B prefix exactly (102 B append). Its following status GET
  lost connectivity. After user-prepared Sync reconnection, a fresh three-read
  series passed with identical5,548,627 B bodies, initial prefix exact and152 B
  retained append. Second original restoration completed; initial failed records
  remain preserved.
- Each reader completed39 stress requests:34 exact complete downloads and five
  intentional interruptions (three TCP resets, two seven-second stalled readers),
  with healthy following requests and no restart. Nine sizes1…1,048,593 B,
  Korean filename, HTTP/1.0 and slow reception are included. No automatic
  request retry masked a failure; intentional incomplete responses are not
  counted as complete downloads.
- Host1135,11 FileDownload tests, nine actual-header fault mutants, strict
  Cppcheck2.11/default/gh_release and app Release protocol99 pass.
- Inter-request free heap minima X3 16,156 B / X4 13,652 B do **not** satisfy
  the project's >50 KiB release memory gate. A successful transfer sample is
  not a whole-product memory sign-off or a promise against future RF failures.
- First restoration completed on both readers, preserving original whole files,
  public/raw/cache state and log append. X4 was then reinstalled for the explicit
  log recheck; its second restoration is now complete, including7 whole original
  books and raw/public/known-cache comparisons. Separate R12 trials may now own
  the readers; consult the current handoff.
  Mechanical power/battery cold-on remain unverified. No publication is authorized.


## Subsequent frozen production regression (R12)

The same eb403... production BIN subsequently passed native reading, ordinary
inactivity/timer resume, continuation and separate Sync HTTP TXT transfer on
both readers; both original active firmware/data restorations are closed.
See [current release evidence](release-checklist.md). This does not exercise
the legacy production File Transfer profile. Its memory gate remains unmet.
The local convenience firmware/update.bin now points to a separate developer
build; the tested immutable candidate stays in R12/artifacts/production.bin.
The staging mismatch and corrected read-only closure audit are explicit evidence.
