# Sync HTTP route ownership

Dedicated COMPANION/POCKET_SYNC sessions no longer construct one Arduino
FunctionRequestHandler, cloned Uri and callback wrapper per Pocket endpoint.
The existing WebServer not-found callback dispatches exact method/path pairs
to the same handlers using request-scoped borrowed data. One generic route
definition feeds both this dispatcher and the unchanged browser registry, so
profile/capability/compiler gates cannot silently diverge between copies.
Root/status use the same status builder; heartbeat still does not extend a
private session's activity timer. `/upload` remains a registered multipart
handler, and port82, JSON bodies, identity checks and storage semantics stay
unchanged. No new HTTP server, task or permanent dispatch table is allocated.

Evidence: the target ELF's DWARF reports FunctionRequestHandler size80B; the
installed SDK constructor additionally clones Uri (owning its String) and
copies callbacks. Eliminating these *persistent* objects is different from
moving request-time allocations later. Exact savings depend on allocator and
enabled routes; do not claim a measured byte gain without hardware comparison.
The dispatcher keeps one request-scoped SDK `uri()` String copy alive while
borrowing it; this is released before deferred presentation admission.

SDK inspection confirms JSON/form arguments are parsed even without a matched
handler; registered `/upload` retains its multipart callback. Pure host tests
cover exact method/path, unknown paths, first-match-only and borrowed lengths;
source checks guard the shared route definitions and wiring. Device HTTP body
handling, saved heap, same-session redraw and repeated Apply remain physical
acceptance gates. The16KiB/4KiB presentation floors are unchanged.

## Sync releases its own TIME_WAIT PCBs (2026-09-24)

Arduino-ESP32 3.3.11 `WebServer::handleClient()` drops `_currentClient` right
after every response, so the reader actively closes each HTTP connection.
lwIP keeps those PCBs in TIME_WAIT for 2*MSL (`CONFIG_LWIP_TCP_MSL=60000`,
120s) on the general heap (`MEMP_MEM_MALLOC`), up to
`CONFIG_LWIP_MAX_ACTIVE_TCP=16`. `CONFIG_LWIP_SO_LINGER` is off.

Evidence on X3 dedicated Sync (build/apply-memory/):
- wbcb431b1: eight `/api/status` reads at 2s spacing lowered free heap
  19,016B -> 17,468B (~256B each); it recovered ~120s after the last read.
  The companion heartbeat (15s) alone keeps ~8 of them; an Apply burst
  (state/status/stream/commit/prepare/activate/present) approaches the cap.
  That matches the failed admissions 16,448B -> 14,184B -> 13,140B.
- A client-closes-first variant (hold the socket until the peer's FIN) was
  built and measured, then removed: macOS drops the reader's FIN once curl
  or URLSession has fully closed its socket, so PCBs sat in LAST_ACK
  retransmitting FIN (~410B each) until the Mac's 60s FIN_WAIT_2 timeout.
- w0482f45b (this change): the same eight reads held 19,136-19,176B; the
  dev census showed timeWait=0, lastAck=0 and the purge counter advancing.

Mechanism: in Sync profiles `ServerTimeWaitPurge` walks `tcp_tw_pcbs` under
the TCPIP core lock every 100ms and aborts PCBs whose local port is this
server's HTTP or upload-stream port. lwIP's `tcp_abandon` (verified in the
linked ELF) removes a TIME_WAIT PCB and frees it without sending a segment.
While a presentation awaits admission it runs on every pass, so the sample
taken right after `handleClient()` never includes this server's closed
sockets. Live and closing connections (`tcp_active_pcbs`) are never touched,
and browser File Transfer keeps the stock behavior.

Trade-off: TIME_WAIT guards a reused 4-tuple against delayed segments from
the old connection. Clients open each request from a fresh ephemeral port on
the local link, and sequence checks still apply, so the residual risk is a
rare reset of a new request, not silent data corruption. The 16KiB/4KiB
presentation floors are unchanged. `GET /api/pocket/v1/dev/tcp` (developer
builds only) reports heap, largest block, the purge counter and PCB states.
