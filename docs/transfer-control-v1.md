# Transfer feedback and cleanup v1

The shared source of truth is the companion app's
[Content and firmware transfers](../../pocket-daily/docs/TRANSFERS.md).

This firmware advertises `transferControl: 1`, accepts bounded prepare/discard
requests for UUID staging paths, and presents transfer state in the existing
Sync/File Transfer activity. Cleanup never removes a published book or update.bin
and never flashes firmware. Physical X3/X4 acceptance remains pending.
