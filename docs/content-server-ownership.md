# Server-loop file writer admission

`CrossPointWebServer::handleClient` services the stream, HTTP parser and
WebSocket callbacks serially on the activity loop. The HTTP parser consumes
multipart/WebDAV bodies synchronously. Stream and legacy WebSocket uploads can
retain writable handles across loop iterations, so sequential callbacks alone
did not prevent overlapping logical writes.

The shared live-writer snapshot now includes HTTP's open upload handle, the
legacy WebSocket in-progress flag and the stream HEADER/DATA phase:

- Stream admission rejects both HTTP and legacy WebSocket writers.
- HTTP multipart START and legacy WebSocket START reject any current writer.
- HTTP create/rename/move/delete and Pocket commit reject live writers with409.
- WebDAV PUT admission and PUT/DELETE/MKCOL/MOVE/COPY reject other live writers
  with409. Read-only operations are unchanged.
- Rejected multipart/WebDAV requests retain rejection through WRITE/END/ABORT.
  They do not replace the shared receipt, close another writer's handle, or run
  partial-file deletion. The next START reevaluates admission.
- A completed stream in REPLIED has already closed its handle. It does not
  block commit, avoiding a race between the final stream acknowledgement and
  the following HTTP commit request. Session end still waits for stream idle.

This policy adds no task, heap allocation, permanent transfer buffer, network
reconfiguration or retry loop. It assumes the existing single server-loop
dispatch; it is not an inter-task mutex. Any future asynchronous storage service
must introduce explicit ownership, not reuse this snapshot as a lock.

## Content integration still required

Sealing/activation handlers must check this writer gate before reading staging,
then keep exclusive server-loop ownership for their entire storage operation.
Do not service upload callbacks or yield into another writer mid-operation.
The read-only preparation inspection handler is now exposed, using this writer
gate and a watchdog progress hook (see `content-prepare-v1.md`). Sealing and
activation handlers remain absent. Watchdog-aware bounded processing,
HAL error classification, quotas, rendering and app receipt integration remain.
The companion now has an internal deployment coordinator (sibling
`docs/CONTENT_DEPLOYMENT.md`), tested with a fake transport. Its preparation
inventory must describe actual verified files in the candidate directory;
read-state must report fully verified recovery or fail. No wire endpoints or
runtime deployment adapter have been added on either side, except for the
preparation inspection endpoint and its Swift receipt decoder.

Host tests cover all writer-bit combinations and per-request rejection lifetime.
The actual handler bindings were source-audited and compiled; simultaneous live
HTTP/WebSocket/WebDAV request sequences still require integration/hardware tests.
This is not evidence that the original Wi-Fi throughput problem is resolved.
