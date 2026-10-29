# Bounded reader performance experiments

The objective is faster reading with unchanged output, stability and memory
headroom. Keep the stable upstream base. Test one downstream optimization at a
time; development/RC imports remain a separate review list.

`scripts/dev_reader_benchmark.py` freezes developer artifacts and unchanged EPUBs,
alternates baseline/candidate walks, retains every result and screens repeatable
gains. It does not generate arbitrary patches, accept a candidate, commit or
publish. Use its measurements to select the next single bottleneck to address.

## Measurement contract

- Both A/B images must include `walkPerformanceVersion: 1`. Each distinct image
  needs its own exact developer version. The manifest records the BIN SHA-256;
  the device confirms installed version/identity/model. That response is **not**
  a readback hash of running flash. Keep the build/source records for both BINs.
- A walk records 2–16 pages, including the opening page. The existing 16-entry
  `ReaderPerf` ring supplies all records. No extra live ring, framebuffer or
  per-page SD write is added. At the terminal checkpoint, after render timing
  completes, at most 800 bytes are written through `HalStorage` before reboot.
- `GET /api/pocket/v1/dev/reader-capture?run=<run>&performance=1` returns a binary
  trace through the existing staging buffer and RenderLock. It uses the existing
  identity/STA admission. All capture/trace endpoints are developer-only.
- PDBM v1 has a 32-byte little-endian header: magic, version, run, page count,
  record size (48), two zero reserved words, FNV-1a checksum of the first 28 bytes.
  Each 48-byte record has 12 uint16 stage durations, 4 uint16 I/O/glyph counters,
  uint8 strip count and flags, uint32 free heap/largest block, uint16 page index,
  and FNV-1a of its first 44 bytes. Checksums detect corruption, not authenticity.
- The host verifies exact run, length, order, checksums, opening/turn sequence,
  nonzero timing/memory and absence of saturated counters. Raw evidence is saved
  before cleanup. Missing/malformed evidence fails the trial and preserves the
  remote receipt; it is never silently excluded from the sample.
- General correctness walks may still use 17–128 pages. They do not expose a
  complete trace and cannot be used as performance samples in this runner.

## Running an experiment

Prepare a model-specific backup and a restore BIN matching the currently
installed developer version. The backup receipt is an external acknowledgement;
the runner does not infer a full SD backup from it. Keep any original recovery
image separately. The reader must be enrolled, available over File Transfer →
Join a Network, with no pending/unrecovered transfers, and the companion app
stopped. A production image must first be put through the separately authorized
developer-install workflow; this runner will not replace production via a
nonexistent developer endpoint.

Freeze inputs offline (paths below are examples, not existing artifacts):

```sh
python3 scripts/dev_reader_benchmark.py plan \
  --config build/dev-pipeline.json --model X3 \
  --baseline /absolute/baseline.bin --candidate /absolute/candidate.bin \
  --restore /absolute/original-developer.bin \
  --backup-receipt /absolute/model-specific-backup-receipt.json \
  --fixture en=/absolute/english.epub --fixture ko=/absolute/korean.epub \
  --pages 16 --repeats 5 --output build/reader-benchmark-x3

python3 scripts/dev_reader_benchmark.py run \
  --plan build/reader-benchmark-x3/plan.json --execute

python3 scripts/dev_reader_benchmark.py compare \
  --plan build/reader-benchmark-x3/plan.json
```

Use a separate X4 plan/config and repeat. Include representative text, CJK and
image fixtures. A short fixture that ends before the requested page count is an
invalid trial; choose a suitable count instead of discarding its failure.

Planning freezes the two measured BINs, restore BIN, config, backup receipt and
all walk plans/EPUBs in a private directory. Running holds the existing pipeline
lock. It checks the installed model/version, no active receipts and unchanged
inputs before device work. The trial order is AB, BA, AB, BA, AB. Each walk uses
a new path with a cold path-keyed cache, starts at the beginning and waits the
existing one-second dwell between forward turns. An unclean run stops the
campaign. Never rerun its `run` command to recover an ambiguous submission.

After clean trials, the runner restores the original developer version and
compares the original public state before computing the speed screen. If a walk
or installation fails, it retains evidence and does not flash over a pending
operation. Inspect `execution.json` and that walk's `report.json`; resolve the
receipt using the existing reader-walk observation/recovery procedure first.
Restoration confirms version and public state, not complete SD/flash equality.

## Screening and the improvement loop

For each fixture, compare at least five paired walks. Treat the walk as the
independent repeat; adjacent pages are correlated. Report opening time separately
from forward-turn p50/p95. A timing screen requires at least 10% p50 improvement
in **every** pair, no more than 5% opening/p95 regression in any pair, no lower
sampled free heap/largest block, and free heap above 50,000 bytes. Both sides
must have matching render-mode flags/strip counts and final BW capture/page.
This is a conservative engineering screen, not a statistical confidence claim.

Use stage durations and SD/glyph counters to choose a mechanism: reduce repeated
storage work if prewarm/page loading dominates, reduce layout work if section
construction dominates, or examine the display path if refresh dominates. Keep
each candidate isolated. Run the required host/static/build checks, freeze the
candidate, execute both model campaigns and retain rejected results. Roll back
source changes only within that candidate's isolated change; never reset a shared
dirty checkout. Do not claim a gain from host wall time or historical numbers.

`automaticAcceptance` and `productionAcceptance` remain false. This first
measurement loop covers final BW output, not every intermediate page, grayscale
panel quality or ghosting. It also excludes physical-button latency, energy,
rapid/backward turns, all four orientations and normal progress-save costs
(history-isolated walks suppress those writes). Heap is sampled during rendering,
not continuously measured peak use. Validate those gates before adopting a
change that could affect them; retain the existing X3/X4 release requirements.

## Current implementation evidence (2026-10-28)

Evidence is in ignored `build/reader-performance-loop-20261028/`. Host tests,
strict cppcheck and default firmware compilation are software evidence only.
ESP32-C3 compiler stack inspection reports 208 bytes for trace writing and 112
for reading; the existing reader-loop and endpoint frames remain 128 and 448
bytes respectively. These are individual compiled frames, not total stack or
runtime heap measurements. Three injected faults (live-ring order, wire index,
accepting zero gain) are rejected by the tests.
The broad Python suite has the existing `test_check_seam` failure for six
unregistered includes in the pre-existing trial/OTA changes; this work does not
suppress or expand those exceptions. Both enrolled readers' saved Wi-Fi
addresses were unreachable with Python and system curl. No new device trial,
measured speedup, optical acceptance or firmware installation is claimed.
