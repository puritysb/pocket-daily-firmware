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
