---
name: debug-crashes
description: Diagnosing firmware crashes, panics, reboots, and hangs on the ESP32-C3. Use when the device resets, panics, freezes, triggers a watchdog, renders corrupt output, or when a stack trace or guru meditation appears in serial output. Covers the five common causes (OOM, stack overflow, use-after-free, corrupt cache, watchdog), the instrumentation for each, and how to confirm a fix.
---

# Debugging Crashes

On this device almost every crash is a resource crash. Before theorising, get the serial log:
`python3 scripts/debugging_monitor.py` with a `LOG_LEVEL=2` build. A crash without a log is a
guess.

## The five causes, in frequency order

**1. Out of memory** — the default assumption.
```cpp
LOG_DBG("MEM", "Free heap: %d bytes", ESP.getFreeHeap());
```
Log it across the activity lifecycle, not once. Look for a large allocation (>10KB) just
before the crash, and for buffers that `onEnter()` allocates but `onExit()` never frees.
Remember fragmentation, not total usage, is what kills this device — free heap can look
healthy while the largest contiguous block is too small.

**2. Stack overflow** — deep recursion or a large local.
```cpp
LOG_DBG("TASK", "Stack high water: %d", uxTaskGetStackHighWaterMark(taskHandle));
```
Under ~512 bytes of headroom, raise the task stack (2048 → 4096, in **bytes**) or move the
buffer to the heap. Locals over 256 bytes belong on the heap regardless.

**3. Use-after-free** — an activity was deleted while its task still ran. Always
`vTaskDelete()` in `onExit()` **before** destruction, and null out pointers after freeing.

**4. Corrupt cache files** — symptoms are wrong layout or missing text rather than a panic.
Delete `.crosspoint/` on the SD card to force a clean re-parse, and check the format versions
(see `lib/Epub/CLAUDE.md` and `docs/file-formats.md`). A version that was changed without a
bump feeds stale caches to the reader silently.

**5. Watchdog timeout** — a loop or task blocked >5s. Add `vTaskDelay(1)` in tight loops and
look for blocking I/O on the main path.

## Confirming a fix

1. Read the serial stack trace — don't stop at the first plausible cause.
2. Log `ESP.getFreeHeap()` before and after the suspect operation, not just at the crash.
3. Verify task teardown with `vTaskList()`.
4. Reproduce with `LOG_LEVEL=2` before and after the change. A crash that merely became rarer
   is not fixed — say so rather than claiming it is.
5. Memory and timing bugs are hardware-verified only. State clearly what you could not test
   here and hand the device check back to the user.
