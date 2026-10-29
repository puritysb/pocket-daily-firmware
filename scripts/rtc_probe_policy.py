"""Pure guards for explicitly reserved X4 RTC experiments; no device I/O."""
MAGIC = 0x50445731
ARENA_START, ARENA_END, SDK_RESERVED = 0x50001700, 0x50001b00, 0x50001fd8
GPIO_TRACE_ADDRESS = 0x50001a80

def need(condition, reason):
    if not condition:
        raise ValueError(reason)


def symbols(text):
    result = {}
    objects = {}
    for line in text.splitlines():
        row = line.split()
        if len(row) != 3:
            continue
        address, kind, name = int(row[0], 16), row[1], row[2]
        result[name] = address
        if kind in 'tTbBdDrR' and 0x50000000 <= address < 0x50002000:
            need(name not in objects, 'Ambiguous native RTC object')
            objects[name] = address
    return result, objects


def validate_layout(original_text, probe_text):
    before, native = symbols(original_text)
    after, _ = symbols(probe_text)
    need(before.get('_rtc_force_slow_end') == ARENA_START and
         before.get('_rtc_reserved_start') == SDK_RESERVED, 'Original RTC baseline differs')
    need(after.get('_rtc_probe_start') == ARENA_START and after.get('_rtc_probe_end') == ARENA_END and
         after.get('_rtc_force_slow_end') == ARENA_END and after.get('_rtc_reserved_start') == SDK_RESERVED,
         'Arena must be explicitly excluded from the SDK RTC heap')
    need(native and all(after.get(name) == address for name, address in native.items()),
         'Existing RTC objects moved or disappeared')
    need(after.get('esp_default_wake_deep_sleep') == 0x50000000,
         'Existing SDK wake stub differs')
    return {'arenaStart': ARENA_START, 'arenaEnd': ARENA_END,
            'runtimeHeapStart': ARENA_END, 'sdkReservedStart': SDK_RESERVED,
            'unchangedNativeRTCObjects': len(native)}


def validate_runtime(report):
    need(type(report) is dict and report.get('version') == 1 and
         report.get('start') == ARENA_START and report.get('end') == ARENA_END and
         report.get('heapReservationEnd') == ARENA_END and report.get('heapIntegrity') is True and
         type(report.get('rtcHeapFree')) is int and 0 <= report['rtcHeapFree'] <= SDK_RESERVED - ARENA_END,
         'Runtime image/heap exclusion is unproven; no memory write allowed')


def validate_timer_trace(words, run, lease):
    need(len(words) == 32 and all(type(v) is int and 0 <= v <= 0xffffffff for v in words),
         'Fixed 32-word trace required')
    need(words[0] == MAGIC and words[1] == run and words[2] == 1 and words[31] == 0,
         'Missing or foreign timer-only wake-stub execution')
    # ESP32-C3 RTC_TIMER_TRIG_EN is bit3. The independent public SDK enum is4.
    need(words[3] == 8 and words[4] == 0, 'Actual first wake is not RTC timer alone')
    need(lease.get('run') == run and lease.get('phase') == 4 and lease.get('failure') == 0 and
         lease.get('resetReason') == 8 and lease.get('wakeCause') == 4,
         'Independent lease deep-sleep/timer return does not match')


def validate_gpio_trace(words, run, lease):
    need(len(words) == 32 and all(type(v) is int and 0 <= v <= 0xffffffff for v in words),
         'Fixed 32-word GPIO trace required')
    need(words[0] == MAGIC and words[1] == run and words[2] == 3 and words[31] == 1,
         'Native GPIO resume did not complete for this run')
    need(words[3] == 8 and words[5] == 4 and words[6] == 8 and words[22] == 0 and
         words[23] == 8 and words[24] == 7 and words[30] == 0,
         'Actual GPIO wake, native boot routing or SDK reset witness differs')
    need(all(words[i] == 1 for i in (7,8,9,10,11)) and words[27] == 48000,
         'Native wake policy, actuator release, frame/position or bitmap proof failed')
    need(lease.get('run') == run and lease.get('phase') == 4 and lease.get('failure') == 0 and
         lease.get('resetReason') == 3, 'Final independent bounded slot return differs')


def validate_gpio_runtime(report):
    validate_runtime(report)
    need(report.get('traceAddress') == GPIO_TRACE_ADDRESS,
         'GPIO trace address differs from the matched payload/consumer ABI')
