import unittest
import rtc_probe_policy as p

BASE = '''50000000 T esp_default_wake_deep_sleep
50000014 B original_one
50001604 B silentRebootPayload
50001608 B silentRebootTarget
5000160c B silentRebootMagic
50001700 A _rtc_force_slow_end
50001fd8 A _rtc_reserved_start
50001fd8 b sdk_retained_timer
'''
PROBE = BASE.replace('50001700 A _rtc_force_slow_end', '''50001700 A _rtc_probe_start
50001b00 A _rtc_probe_end
50001b00 A _rtc_force_slow_end''')

class RTCProbePolicy(unittest.TestCase):
    def test_explicit_reservation_retains_every_native_object(self):
        result = p.validate_layout(BASE, PROBE)
        self.assertEqual(result['unchangedNativeRTCObjects'], 6)
        self.assertEqual(result['runtimeHeapStart'], p.ARENA_END)

    def test_elf_empty_gap_is_not_heap_exclusion(self):
        with self.assertRaises(ValueError): p.validate_layout(BASE, BASE)
        unsafe = PROBE.replace('50001b00 A _rtc_force_slow_end', '50001700 A _rtc_force_slow_end')
        with self.assertRaises(ValueError): p.validate_layout(BASE, unsafe)

    def test_changed_native_rtc_address_is_refused(self):
        for name in ('original_one','silentRebootMagic','sdk_retained_timer'):
            rows = PROBE.splitlines()
            bad = '\n'.join('50001900 B '+name if x.endswith(' '+name) else x for x in rows)
            with self.assertRaises(ValueError): p.validate_layout(BASE, bad)

    def test_runtime_heap_proof_is_mandatory(self):
        good = dict(version=1,start=p.ARENA_START,end=p.ARENA_END,heapReservationEnd=p.ARENA_END,
                    heapIntegrity=True,rtcHeapFree=384)
        p.validate_runtime(good)
        for key, bad in [('heapIntegrity',False),('heapReservationEnd',p.ARENA_START),
                         ('start',p.ARENA_START+16),('end',p.SDK_RESERVED+4),('rtcHeapFree',99999)]:
            with self.assertRaises(ValueError):p.validate_runtime({**good,key:bad})
        with self.assertRaises(ValueError):p.validate_runtime(None)

    def test_timer_execution_requires_independent_hardware_witness(self):
        words = [0]*32
        words[:5] = [p.MAGIC,719,1,8,0]
        lease=dict(run=719,phase=4,failure=0,resetReason=8,wakeCause=4)
        p.validate_timer_trace(words,719,lease)
        for index, value in [(0,0),(1,718),(2,0),(3,0),(3,8|1),(4,8),(31,1)]:
            changed=words.copy();changed[index]=value
            with self.assertRaises(ValueError):p.validate_timer_trace(changed,719,lease)
        for key,value in [('phase',5),('failure',8),('resetReason',3),('wakeCause',7),('run',718)]:
            with self.assertRaises(ValueError):p.validate_timer_trace(words,719,{**lease,key:value})

    def test_gpio_runtime_requires_consumer_trace_address(self):
        good=dict(version=1,start=p.ARENA_START,end=p.ARENA_END,heapReservationEnd=p.ARENA_END,
                  heapIntegrity=True,rtcHeapFree=400,traceAddress=p.GPIO_TRACE_ADDRESS)
        p.validate_gpio_runtime(good)
        with self.assertRaises(ValueError):p.validate_gpio_runtime({**good,'traceAddress':p.GPIO_TRACE_ADDRESS-128})

    def test_native_gpio_pass_requires_every_independent_flag(self):
        words=[0]*32
        words[0]=p.MAGIC;words[1]=891;words[2]=3;words[3]=8;words[5]=4;words[6]=8
        for i in (7,8,9,10,11):words[i]=1
        words[23]=8;words[24]=7;words[27]=48000;words[31]=1
        lease=dict(run=891,phase=4,failure=0,resetReason=3)
        p.validate_gpio_trace(words,891,lease)
        for index,value in [(0,0),(1,890),(2,2),(3,4),(5,8),(6,0),(7,0),(8,0),(9,0),
                            (10,0),(11,0),(22,1),(23,3),(24,4),(27,52272),(30,1),(31,0)]:
            bad=words.copy();bad[index]=value
            with self.assertRaises(ValueError):p.validate_gpio_trace(bad,891,lease)
        for key,value in [('run',890),('phase',5),('failure',8),('resetReason',8)]:
            with self.assertRaises(ValueError):p.validate_gpio_trace(words,891,{**lease,key:value})

if __name__ == '__main__':unittest.main()
