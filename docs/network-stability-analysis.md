# Reader network stability — root-cause analysis and core options

STATUS: ACTIVE WORKSTREAM, opened 2026-09-19 evening after a full day of
degrading radio behavior. This document is the evidence ledger, hypothesis
register, experiment matrix, and the decision framework for how deep the
fix goes (targeted hardening vs core reimplementation).

## Stack under analysis

- ESP32-C3, no PSRAM, 380 KB heap. Arduino framework (not native IDF).
- Platform pioarduino 55.03.37, arduino-esp32 core 3.3.7 (ESP-IDF 5.3 line).
- Server: Arduino WebServer + links2004 WebSockets + custom port-82 TCP
  stream, all serviced from the single activity loop.
- File Transfer STA boot baseline: 13-16 KB free on LS-2-era builds,
  ~10 KB on LS-3 builds.

## Evidence ledger (2026-09-19, one device, one router)

| Time (approx) | Observation | Conditions |
|---|---|---|
| morning | 6 MB transfers complete repeatedly; hours of stable session | LS-1/LS-2 builds; heap 13-16 KB; rssi -41..-76 swinging |
| midday | Loop crawl (~30 s/pass) until power cycle | links2004 broadcastTXT blocked on dead WS peer + 15 s keepalive feeding it. FIXED (`c385fc8b`), verified |
| afternoon | Zombie: `WL_CONNECTED` but deaf (ARP dead); recovery only by mode re-entry at first, then not even that | after heavy chunk-per-connection hammering; crash report unchanged (4,245 B - no panics all day) |
| evening | Mode re-entry recovery window shrinks to ONE request; then cold boots yield 1-2 requests before death | LS-3 builds, heap ~10 KB at boot |
| late evening | After ROUTER reboot: reader reassociates on a new lease, sustained push kills the radio again | `fbadf188` never reached the device; the running build still had the WS listener active (12 KB gate passed at begin) |

Key facts that constrain the hypothesis space:

- Death is at L2 (ARP unanswered), not TCP: the radio/driver stops
  servicing frames while association appears up.
- No panics/watchdogs all day (crash report byte-identical).
- E-ink screen and keys keep working when the radio is dead - only the
  network dies.
- Recovery requires a full chip reset; `WiFi.begin` re-run inside mode
  re-entry does NOT recover it. This implicates state inside the WiFi
  driver/lwIP that survives a `WiFi.disconnect`, not application state.
- Morning (13-16 KB heap) sustained 6 MB transfers; evening (10 KB heap)
  died at the start of them. The heap baseline is the variable that moved
  with build generation, and death threshold moved with it.

## Hypothesis register (ranked)

H1 - **WiFi driver RX/TX buffer starvation under heap pressure.**
On a PSRAM-less C3 the WiFi driver allocates buffers from the same heap
as everything else. At ~10 KB free with fragmentation, a sustained TCP
burst leaves the driver unable to allocate RX buffers; it goes deaf at L2
while the association state machine stays "connected". Cold boot resets
heap; mode re-entry does not free enough. Explains: L2 deafness, the
morning/evening threshold shift with heap baseline, recovery only by
reset. Partially explains evening-cold-boot decay (fragmentation rebuilds
faster under congestion?).

H2 - Router/AP-side state accumulation. Router reboot did NOT fix the
sustained-transfer death, so at most a contributor, not the cause.

H3 - 2.4 GHz evening congestion (rssi swings all day). Would explain
variance, not L2 deafness of the client itself.

H4 - arduino-esp32 3.3.7 / IDF 5.3 driver defect class (C3 deafness
reports exist across IDF versions). Plausible multiplier; test by
upgrading the platform after H1 is addressed.

H5 - Power delivery dips during TX bursts (X3 hardware). Would brownout,
not deafen; reset reason never showed brownout. Low.

## Experiment matrix

Each experiment isolates one variable. All use instrumented builds (below).

- E1 Router isolation: reader in Create Hotspot mode, Mac joins the reader
  AP directly, sustained 6 MB push. No router in path.
  - Dies -> router exonerated further, H1/H4 strengthened.
  - Survives -> H2/H3 back on the table.
- E2 Heap isolation: a heap-rich build (WS listener compiled out, builtin
  fonts omitted, ~20+ KB baseline) does the same sustained push over STA.
  - Survives where the 10 KB build died -> H1 confirmed at the threshold
    level; heap diet is THE fix; no core rewrite needed.
- E3 Time-of-day: repeat the 10 KB-build push in the morning.
  Separates congestion (H3) from heap (H1).
- E4 Post-mortem capture: run the failing scenario with instrumentation
  and read the SD log after death (the log survives the radio).

Decision gate after E1-E4:

- H1 confirmed -> HN-2 heap diet + HN-4 lwIP tuning + HN-1 health monitor;
  no core rewrite. (Days of work.)
- H1 refuted and H4 implicated -> platform/IDF upgrade trial, then
  evaluate the native-IDF migration (Option 2 below).
- Inconclusive -> add driver-level stats (esp_wifi internal counters) and
  repeat.

## Instrumentation (build first, always on, tiny)

- WiFi event hook (Arduino `WiFi.onEvent`): log every
  STA_DISCONNECTED/CONNECTED with the 802.11 reason code to
  `/.crosspoint/net-health.log` on SD.
- 60 s heartbeat line: uptime, freeHeap, largest free block, rssi.
- On the activity loop's "wifi not connected" branch and on every
  accept()/send() timeout: one line each.
- Bounded: rotate the file at 64 KB (rename to .old, start fresh).
- Dev endpoint `GET /api/pocket/v1/dev/net-health` (and it must be
  readable after re-entry even when the radio died earlier).

The reason codes on disconnect are the single most valuable datum: beacon
timeout vs AP-driven vs local failures point at different layers.

## Core options (if analysis says go deeper)

- Option 0 - Targeted hardening (HN-1..HN-4 as already planned).
  Cost: days. Covers H1-H3 outcomes. Default.
- Option 1 - Network service rewrite: move ALL network servicing (HTTP,
  WS, port-82 stream) into a dedicated FreeRTOS task with a queue; the
  activity loop only renders and never blocks on sockets. Keeps Arduino
  core. Cost: 1-2 weeks. Fixes the "blocking send freezes the reader"
  class structurally and isolates WiFi memory pressure from UI state.
- Option 2 - Native ESP-IDF migration for the network layer (drop the
  Arduino core entirely or hybrid): full control of lwIP buffers, WiFi
  event handling, and power management. Cost: months, touches everything.
  Only justified if H4 (driver/framework defect) is proven and no upgrade
  fixes it.
- Option 3 - Protocol slimming independent of the above: single
  persistent connection per session (no per-chunk HTTP), UDP-based
  discovery, binary framing instead of JSON. Reduces socket churn and
  lwIP pressure regardless of the chosen option. Cost: days-weeks, mostly
  app-side.

Recommended sequence: instrument -> E1-E4 -> almost certainly Option 0 +
Option 3, with Option 1 as the structural follow-up. Option 2 only on
proven framework defect.
