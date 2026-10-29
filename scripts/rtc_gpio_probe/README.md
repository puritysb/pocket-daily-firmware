# Nonshipping X4 GPIO wake payload

`build.py` is offline only. It compiles this standalone C3 ROM wake stub, checks
that an explicitly reserved diagnostic ELF retains every native RTC object, and
emits binary, disassembly and hashes in a fresh output directory. It cannot
connect, provision, assert a pin, install or reset a reader.

Use the repository C3 command database and exact frozen baseline/probe ELF files:

```sh
python3 scripts/rtc_gpio_probe/build.py \
  --command-database compile_commands.json \
  --baseline-elf /private/baseline/firmware.elf \
  --probe-elf /private/rtc-probe/firmware.elf \
  --output /private/fresh-output
```

The tested payload is820 bytes. Runtime provisioning must separately prove the
same enrolled X4, two matching diagnostic images, healthy heap before/after,
exclusive debugger ownership, unchanged CPU/pin registers and an owned run.
An ELF gap is insufficient: the SDK allocates RTC tail space as heap unless the
linker reservation advances `_rtc_force_slow_end`. See
[the trial contract](../../docs/developer-trial-lease.md).

The native sleep path holds GPIO3 as an input. Only the observed native hold is
released before open-drain LOW; every other pin remains untouched. The first
wake must be timer, and the second must have actual RTC GPIO cause and GPIO3
mask. A future RTC alarm and awake watchdog bound failure. The matched
nonshipping firmware releases the actuator through the HAL after unchanged
native wake policy and records a normally rendered reader page and position.
Timer/watchdog fallback cannot satisfy the GPIO receipt predicate.

R6 trial006 passed electrical GPIO wake and native reader resume on a USB-powered
X4. It does not certify mechanical switch contacts, battery-latch cold-on,
current, latency, optical pixels or shipping-candidate acceptance. Raw register
access here is confined to a freestanding pre-application RTC payload; it is
never compiled into shipping firmware or used through a normal activity.
