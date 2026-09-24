# Developer update return mode

The dev-only remote-flash endpoint now records the current server profile and
bearer in the existing one-shot marker before flashing. It closes and rereads
that one-byte record; failed persistence returns503 without flashing. Firmware
validation and explicit installation approval remain unchanged.

| Marker | Return target |
| --- | --- |
| `1` (legacy) | File Transfer, one saved-STA association attempt |
| `S` | Pocket Sync, one saved-STA association attempt |
| `F` | File Transfer chooser, no automatic network change |
| `N` | Pocket Sync chooser, new explicit Nearby pairing required |

An AP session never silently switches to infrastructure Wi-Fi or persists its
temporary BLE/AP credentials. Empty, oversized, unknown or unreadable records
do not start a radio. Removal must succeed before automatic restoration; crash
and recovery boots consume the marker but retain their higher-priority routes.
Saved-network failure falls back to the existing interactive Wi-Fi UI, not a
reboot/retry loop. No new credential store, task or framebuffer is introduced.

Compatibility: old developer firmware writes only `1`; its original profile
cannot be reconstructed. The first upgrade from such firmware still returns
to File Transfer. Preserved Sync return applies when the initiating firmware
also has this change and the destination understands these markers. A downgrade
to an older build may revert to its legacy File Transfer return behavior.
Production firmware still has no developer flash endpoint or marker consumer.

Local policy/source tests and compilation are not physical association or
post-update profile verification. A hardware test must start in dedicated Sync,
explicitly install a compatible developer image, verify exact version/identity,
and confirm poll-only Sync on the same saved LAN without menu selection.
