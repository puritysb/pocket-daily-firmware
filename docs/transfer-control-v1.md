# Transfer feedback and cleanup v1

The shared source of truth is the companion app's
[Content and firmware transfers](../../pocket-daily/docs/TRANSFERS.md).

This firmware advertises `transferControl: 1`, accepts bounded prepare/discard
requests for UUID staging paths, and presents transfer state in the existing
Sync/File Transfer activity. Cleanup never removes a published book or update.bin
and never flashes firmware. X3 Mac/app acceptance is recorded below; X4 remains pending.


`/api/status` optionally reports `transferState` version 1 with the same phase,
kind and bounded percentage used by the Sync transfer screen. See the shared
contract above for exact fields. Content Saved/Removed feedback returns to the
underlying Sync screen after five seconds without ending the network session.
Paused/Failed and all firmware feedback remain visible; firmware is never flashed
by this timeout. The unsigned millisecond delta handles timer wraparound. No file
names, new network service, framebuffer or dynamically allocated timer are added.

2026-10-09: user-authorized X3 installation of `ble-standby-we19d4702` verified.
Developer pipeline `20261009T065715-2ce1a5` passed generated EPUB byte equality,
cleanup, idle feedback recovery observed by the app heartbeat and status API,
and standby/BLE reconnection (13.79 s). This is Mac/X3 evidence, not physical
screen-camera, iPhone or X4 sign-off. Candidate SHA-256:
`234ee892ec6eb53ac02fbdc43388495d9811d7c9b99a45474b282be5f38d862d`.
