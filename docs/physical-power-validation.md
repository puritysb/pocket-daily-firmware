# Physical power validation: fixture admission and evidence

The production candidate remains
`da37f7fc86d1c773bc87a7182b1c284bc3de16ceebc4a313c8519076e90487ad`.
R9 proved natural inactivity and native reader continuation after a maintenance
RTC wake on X3/X4. It did not prove a mechanical switch or battery cold-on.

## Current autonomous execution boundary — 2026-10-28

`build/physical-power-preflight-20261028` records a fresh read-only inventory.
The Mac exposes one ESP USB JTAG/serial device; no admitted mechanical actuator,
independently switched reader supply or rail-measurement fixture is available.
`uhubctl` is absent; no project relay/servo binding or fixture driver was found.
No serial port was opened, unknown USB command sent, reader power cut, firmware
installed or user file changed. This is a blocked physical test, not a pass.
The original active firmware and Sync state are checked over enrolled Wi-Fi.

Relevant implementation boundaries:

- `lib/hal/HalPowerManager.cpp:117`: normal sleep drops GPIO13. On X4 this is
  the battery CPU latch; on X3 it is the SD rail, not the CPU supply.
  The SDK board profiles independently describe those different power roles.
- `lib/hal/HalPowerManager.cpp:189`: a recovery timer requires USB on X4.
  CPU code and an RTC stub cannot drive anything after the CPU supply is lost.
- `src/pocket_daily/boot/TrialLease.cpp:245`: without an armable recovery timer,
  the active trial restarts instead of intentionally stranding a battery-off X4.
- `src/pocket_daily/boot/TrialLease.cpp:192`: the shipping trial currently admits
  one checked timer continuation. Other resets return to the preserved slot;
  a physical GPIO wake/cold boot is not automatically a native production pass.
- The earlier `scripts/rtc_gpio_probe` uses a nonshipping electrical GPIO
  actuator on a powered X4. It cannot exercise mechanical contacts or restore
  a CPU whose supply has been cut. Its results cannot satisfy these gates.

Do not remove the timer recovery or force battery-off while no independent
fixture can restore power. Wi-Fi/BLE disconnection, USB de-enumeration,
noncharging status, SD power-off and software reset are not CPU rail-loss proof.
A missing HTTP response alone establishes none of those physical conditions.

## Required fixture and bounded execution sequence

A future admitted external controller needs all of the following for each
reader: a mechanically coupled button actuator with independent press/release
feedback; independently controlled USB VBUS and data/debug back-power isolation;
battery supply control and CPU-rail/battery-source measurement. It needs a
reviewed driver bound to its physical identity and the enrolled reader. A USB
product label or static capability manifest is insufficient admission.

For X3 cold-on, normal sleep's SD rail switch is insufficient: the fixture must
actually remove the CPU supply. For X4 battery cold-on, the fixture must prove
absence of USB/debug power and loss of the CPU rail after latch-off, then
mechanically restore boot from the battery. Rail thresholds, discharge dwell
and current thresholds must come from characterized wiring/calibration; this
repository does not invent electrical limits from a noncharging flag.

Before any power action, the live fixture must prove independent rescue:
release the button, restore a known supply and reconnect without reader code.
It must have its own deadline and safe default outputs after host failure.
The host must verify the exact candidate, enrolled reader, original image/slot
backup and known SD backup. It must stop writes, flush/unmount normally and
finish the sleep transaction before supply removal. Do not inject SD write or
power faults merely to obtain a cold boot.

A dedicated explicit physical trial will then need a one-use NVS continuation
for GPIO or power-on, matched to its run and image, plus an externally guaranteed
recovery window. Cold-on cannot rely on an RTC-only ticket. This extension is
not implemented or installed without an admitted fixture; changing the frozen
shipping candidate now would invalidate its exact-BIN acceptance unnecessarily.
Any resulting candidate must repeat R9 and both physical hardware rows.

The mechanical wake case must observe ordinary sleep, physical press, actual
SDK GPIO/deep-sleep classification, native saved-page render, completed next
page and button release. The cold-on case must additionally observe VBUS absent,
CPU rail off, a physical press, battery-powered boot and actual SDK power-on
classification. Every case ends with original firmware/settings/books/cache
restoration, independent comparison and all fixture outputs restored/released.

## Offline evidence consistency checker

`scripts/check_physical_power_evidence.py` validates bounded raw-file references,
run/model/candidate binding, event ordering, independent press feedback,
scenario-specific reset/power evidence, reading completion and restoration.

```sh
python3 scripts/check_physical_power_evidence.py \
  --receipt /private/run/receipt.json --model X3 --run <32-lowercase-hex-nonce> \
  --candidate-sha256 da37f7fc86d1c773bc87a7182b1c284bc3de16ceebc4a313c8519076e90487ad
PYTHONPATH=scripts python3 -m unittest scripts/test_check_physical_power_evidence.py
```

This checker performs **no hardware I/O**. A consistent JSON plus hash does not
establish that a capture is physical or calibrated; its result always says
`hardwareSignoff:false`. Synthetic test fixtures never become device evidence.
A live fixture/observer and review of measurement provenance remain required.
There is no actuation runner or admitted adapter yet. This artifact is a tested
acceptance guard and implementation contract, not completed physical automation.
