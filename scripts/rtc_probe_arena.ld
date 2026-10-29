/* Explicit diagnostic environments only. Keep every existing RTC object at its
 * original address and exclude this arena from IDF's runtime RTC heap. */
SECTIONS
{
  .rtc.probe (ADDR(.rtc.force_slow) + SIZEOF(.rtc.force_slow)) (NOLOAD) :
  {
    . = ALIGN(16);
    _rtc_probe_start = ABSOLUTE(.);
    . += 1024;
    _rtc_probe_end = ABSOLUTE(.);
    _rtc_force_slow_end = ABSOLUTE(.);
  } > rtc_force_slow_seg
}
INSERT AFTER .rtc.force_slow;
ASSERT(_rtc_probe_start == 0x50001700, "Review changed C3 RTC layout before probing")
ASSERT(_rtc_probe_end == 0x50001b00, "RTC probe arena size changed")
ASSERT(_rtc_probe_end <= _rtc_reserved_start, "RTC probe overlaps SDK retained data")
