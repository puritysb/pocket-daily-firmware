# Core connectivity repair — 2026-09-21

This is a source-backed intervention, not a claim that the running audit image
has been repaired. All hardware tests must keep the Mac on its current network;
USB and a reader hotspot are unavailable for this user.

## Confirmed structural issues

1. `PocketEndpoints.cpp` formerly tied remote flashing to a permanent 4096-byte
   network-log buffer and the live-debug trace ring. `DevTrace.h` reserves 48
   entries, independently of whether a diagnostics request is ever made.
   `NetHealth` also adds an event listener and storage activity. Default developer
   firmware therefore carried diagnostic costs just to enable the update loop.
   These are now gated by `ENABLE_DEV_NETWORK_DIAGNOSTICS`; use the explicit
   `network_diagnostics` environment to restore them. The normal developer
   build retains remote flashing and automatic saved-network return.
   Default build static RAM fell from 139656 to 135160 bytes (4496 bytes).
   This is link-time accounting, not a measured runtime heap/radio result.
2. Installed Arduino `WiFiGeneric.cpp` lines 275–289 initializes the Wi-Fi
   configuration at runtime: static RX 4, dynamic RX 32, dynamic TX 32, dynamic
   TX mode, and TX cache 4. The earlier statement that buffer counts could not
   be changed without rebuilding the precompiled SDK was incorrect. The SDK
   checked here reports ESP-IDF 5.5.5, not the 5.3-era estimate in older notes.

## Upload receive path (all builds)

The installed SDK's `NetworkClientRxBuffer::fillBuffer` lazily mallocs 1436
bytes, then copies into the upload plane's existing buffer. The upload socket
now exclusively receives with nonblocking `recv` into its existing header/SD
buffers, avoiding that allocation and extra copy. NetworkClient still owns the
descriptor, so socket cleanup remains unchanged. Never mix buffered Arduino
reads with this direct receive path on that connection.

The SDK's `NetworkClient::connected()` also reports true when its peek recv
returns zero (orderly EOF). The upload plane now detects EOF directly, drains
queued bytes first, and closes/saves the received prefix immediately instead
of waiting for its 30-second idle timer. Host socket-pair tests cover pending
reads, non-consuming peek, queued data followed by EOF, and descriptor errors.
This is a confirmed receive-path improvement, not proof of the old device's
whole-radio failure mechanism.

## Opt-in core experiment

`sta_recovery` adds a project-owned linker wrapper around `esp_wifi_init`.
It bounds dynamic RX/TX to eight buffers each, static RX to four, and block-ACK
RX window to four or the smaller static RX budget. The incoming ABI structure,
callbacks, magic and unrelated options are preserved. No heap allocation or
installed-framework patch is introduced. Default/release do not use the wrapper.

This bounds potential burst allocations, **not** the whole driver's memory.
Arduino already uses four static RX buffers: no static-RX baseline saving is
claimed. Fewer dynamic buffers can trade throughput for lower peak allocation
pressure; actual heap headroom, packet loss and radio reliability require
hardware measurement. The policy follows the buffer/window relationships in
[Espressif's Wi-Fi guide](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/api-guides/wifi.html).

## What is still unproven

- Small status replies succeeded with roughly 5–9 KiB free on the old audit
  image, while uploads timed out. This supports investigating memory pressure
  and service starvation, but does not identify one proven driver defect.
- 131072 was a **sender socket-submission** counter, not a receiver SD offset.
  It must not be used as proof of a deterministic 128 KiB SD/protocol boundary.
- The old audit image does not contain these changes. Repeated transfer retries
  cannot patch a device whose data path cannot sustain the first installation.
- Dedicated network tasks/native-IDF migration remain alternatives, not current
  fixes: additional task stacks can worsen the immediate memory constraint.

Acceptance: compare default versus sta_recovery on the same X3/network using
1 KiB then 6 MiB transfers, final CRC/commit, repeated reconnects, idle dwell,
and exact post-install version. X4 and other Wi-Fi modes also need regression
checks before promoting the experimental driver policy.

Local verification before the receive-path follow-up: all four environments built; 164 host tests and 16 Python
tests passed. Strict cppcheck passed for default and sta_recovery using the
current project flags with a local native tool-package substitution. Disassembly
confirmed Arduino wifiLowLevelInit calls the wrapper in sta_recovery; the default
ELF has no wrapper. The diagnostics ELF contains the 4096-byte tail and 384-byte
trace ring that are absent from default. None of this proves physical recovery.

The receive-path follow-up increases the host suite to 166 passing tests
(including a real local socket pair). The 16 Python tests still pass. The
physical audit-image probe completed its RESUME handshake and submitted 1024
bytes from the Mac, but never received final OK/CRC. It did not publish a file
or verify SD storage; this was the old-image bootstrap failure, before the SD install below.
Default and sta_recovery builds and strict cppcheck both pass with the new
receive path; the test uses the same nonblocking receive helper as firmware.

SD bootstrap staged (2026-09-21): copied the archived sta_recovery image built
at 08:04:33 to the reader card's `/update.bin` after preserving the previous
image in `firmware-backups/`. Read-back SHA-256 matched
`4156ddd831e1b818e20d20b89e7f2c8a12f7cdf286e3281e38c69e23435a0312`.
Embedded version: `1.6.6-dev-main-b8e38e39-sta-recovery-wf3e32596`.
After the user installed from SD and rejoined the home network, `/api/status`
reported this exact version, uptime 27 seconds, freeHeap 9156, and
`uploadStreamWindow:4096`. A 1024-byte stream returned `OK 1024 48D7F063`.
This verifies installation and small-file reception on the experimental image.
The latest generic `firmware/update.bin` may be a different
environment, so use the archived artifact identity above for this test.

Full-size physical staging subsequently passed: `OK 6030352 AE2A4752`, followed
by a size/CRC-checked HTTP commit publishing `/update.bin`. The first unpaced
sender was deliberately interrupted; its retained 1,187,840-byte prefix was
accepted on reconnect. The resumed sender used `--flow-delay-ms 10` (512-byte
fragments within each 4096-byte SD-credit window) and averaged 10.7 KiB/s over
the remainder, with no further retry. Post-transfer status retained the same
identity/version, uptime 880 seconds and freeHeap 9036. The device remained in
File Transfer STA at the same location, with no Mac network switch.

This establishes one successful large CLI transfer and intentional-interruption
resume on the experimental image. It does not isolate which combined change
fixed reception, establish a pacing-only speed improvement, validate X4, or
prove long-term stability. No redundant flash was performed on the identical
version. Remote flashing a distinct build and automatic network return remain
hardware gates. The app's matching paced path passes 80 tests and both platform
builds; physical app-driven delivery remains open. Python now has 17 passing
tests, including paced-credit-boundary coverage.

## Resource-lifecycle follow-up and wireless installation

File Transfer and Calibre now release the selected SD family with a render lock
before starting Wi-Fi, retaining the defensive release before web-server startup.
The provider's separate 4096-byte OTA decode buffer is removed: synchronous
decode/hash operations borrow the common flasher buffer, completing before its
validation/flash use. No radio caps, sender timing, wire protocol, dashboard
state, or task-stack size changed in this follow-up. See
[UPSTREAM_CONNECTIVITY_COMPARISON.md](UPSTREAM_CONNECTIVITY_COMPARISON.md).

Candidate archived image: `firmware/pocket-daily-1.6.6-sta_recovery-b8e38e39-20260921-213352.bin`,
6,030,416 bytes. Embedded version:
`1.6.6-dev-main-b8e38e39-sta-recovery-w67fff974`.
SHA-256: `40131dfa3f4e8c6f690cb9cfda4a69973253d25ecaf228b125a5eb0275158fa9`.
On 2026-09-21, this distinct image passed wireless reception
(`OK 6030416 4FFA1BE2`), commit, explicit developer flash and same-reader
exact-version verification after automatic saved-Wi-Fi return. No SD swap,
USB, hotspot or post-flash menu action was needed. This supersedes the earlier
remote-flash/rejoin hardware gate for CLI/X3 only.

The sender was deliberately restarted twice to compare/revert pacing, resuming
at 2043904 and 3149824 bytes. The final 512-byte/10 ms paced segment averaged
2.4 KiB/s. Local evidence: `build/resource-device-*.log`. This is not a controlled
pacing comparison or an automatic outage-recovery test. A post-install 65536-byte
staged upload passed CRC B11DE6A1 in 30.44 seconds (about 2.2 KiB/s). The hidden
`.pocket-postinstall-smoke-20260921.part` was not published and remains eligible
for normal stale-staging cleanup on a subsequent root-directory upload.

Status at uptime 33 seconds showed 10960 B free heap; at 187 seconds, 9116 B.
The link-time 4096-byte saving is not a fixed runtime gain. Crash-report
retrieval initially returned HTTP 503 from the low-memory guard. After the smoke
upload, uptime reached 337 seconds without another restart and heap was 10976 B
(Live Studio poll mode). Retrieval then succeeded: the saved panic belongs to
older version `1.6.6-dev-main-8864e736-wec1d3ece`, not this installation. Low heap,
slow throughput, long-run stability, X4 and physical app delivery remain open.
This is successful developer installation, not release sign-off.
Default and sta_recovery builds, strict cppcheck for both environments, all 166
host tests and 17 Python tests passed. The new buffer-sharing contract is checked
by source lifecycle review and the firmware capacity assertion, not by a new
physical provider-OTA test. App code was not changed in this follow-up.
