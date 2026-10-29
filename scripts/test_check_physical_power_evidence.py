import copy
import hashlib
from pathlib import Path
import tempfile
import unittest

import check_physical_power_evidence as power


class PhysicalEvidence(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        data = b'SYNTHETIC TEST DATA: not an actual instrument capture\n'
        (self.root/'measurement.bin').write_bytes(data)
        self.receipt = dict(schema=1, model='X3', run='1a'*16, candidateSHA256='ab'*32,
                            case='mechanical_button_wake', actuator='external_mechanical',
                            pressFeedback='independent_sensor', buttonReleased=True,
                            resetReason='deep_sleep', wakeCause='gpio', samePosition=True,
                            nextRenderCompleted=True, originalRestored=True,
                            rawEvidence={'path':'measurement.bin','sha256':hashlib.sha256(data).hexdigest()})
        self.events(['sleep_committed','mechanical_press','native_reader_render','next_page_render','restored'])

    def events(self, kinds):
        self.receipt['events'] = [{'kind':k, 'monotonicMs':i*1000} for i,k in enumerate(kinds)]

    def check(self):
        return power.check(self.receipt, model='X3', run='1a'*16, candidate_sha256='ab'*32,
                           evidence_root=self.root)

    def cold(self):
        self.receipt.update(case='battery_cold_on', vbusPresent=False, debugBackPower=False,
                            cpuRailOffMeasured=True, batterySupplyMeasured=True,
                            resetReason='power_on', wakeCause='undefined')
        self.events(['vbus_absent','cpu_rail_off','mechanical_press','battery_boot',
                     'native_reader_render','next_page_render','restored'])

    def reject(self, key, values):
        original = copy.deepcopy(self.receipt)
        for value in values:
            with self.subTest(key=key, value=value):
                self.receipt = {**copy.deepcopy(original), key:value}
                with self.assertRaises(power.EvidenceError): self.check()
        self.receipt = original

    def test_malformed_records_fail_safely(self):
        self.reject('schema', [True, '1', None])
        self.reject('rawEvidence', [[], None, 1])
        self.reject('events', [[None], [1], 'events'])
        with self.assertRaises(power.EvidenceError):
            power.check([], model='X3', run='1a'*16, candidate_sha256='ab'*32, evidence_root=self.root)

    def test_consistency_is_never_physical_signoff(self):
        self.assertFalse(self.check()['hardwareSignoff'])
        self.cold()
        self.assertFalse(self.check()['hardwareSignoff'])

    def test_internal_gpio_or_command_is_not_mechanical_press(self):
        self.reject('actuator', ['internal_gpio','software','usb_reset',None])
        self.reject('pressFeedback', ['command_ack','same_gpio',None])
        self.reject('buttonReleased', [False,1,None])

    def test_timer_and_restart_cannot_pass_button_wake(self):
        self.reject('wakeCause', ['timer','undefined',None])
        self.reject('resetReason', ['software','power_on',None])

    def test_usb_and_debug_backpower_cannot_pass_battery_only(self):
        self.cold()
        self.reject('vbusPresent', [True,0,None])
        self.reject('debugBackPower', [True,0,None])

    def test_sd_power_or_noncharging_is_not_cpu_power_loss(self):
        self.cold()
        self.reject('cpuRailOffMeasured', [False,'sd_rail_only',1,None])
        self.reject('batterySupplyMeasured', [False,1,None])

    def test_cold_requires_actual_power_on_classification(self):
        self.cold()
        self.reject('resetReason', ['deep_sleep','software',None])
        self.reject('wakeCause', ['timer','gpio',None])

    def test_run_model_image_are_bound(self):
        self.reject('run', ['2a'*16,None])
        self.reject('model', ['X4',None])
        self.reject('candidateSHA256', ['cd'*32,None])

    def test_missing_or_reordered_events_fail(self):
        good = copy.deepcopy(self.receipt['events'])
        self.reject('events', [good[:-1], list(reversed(good)), []])
        self.receipt['events'][1]['monotonicMs'] = 0
        with self.assertRaises(power.EvidenceError): self.check()

    def test_reading_and_restoration_must_be_complete(self):
        for key in ('samePosition','nextRenderCompleted','originalRestored'):
            self.reject(key, [False,1,None])

    def test_missing_or_changed_raw_capture_fails(self):
        (self.root/'measurement.bin').write_bytes(b'different')
        with self.assertRaises(power.EvidenceError): self.check()
        (self.root/'measurement.bin').unlink()
        with self.assertRaises(power.EvidenceError): self.check()

    def test_raw_path_escape_and_symlinks_are_refused(self):
        self.receipt['rawEvidence']['path'] = '../secret'
        with self.assertRaises(power.EvidenceError): self.check()
        self.receipt['rawEvidence']['path'] = '/etc/hosts'
        with self.assertRaises(power.EvidenceError): self.check()
        (self.root/'outside').symlink_to('/etc/hosts')
        self.receipt['rawEvidence']['path'] = 'outside'
        with self.assertRaises(power.EvidenceError): self.check()


if __name__ == '__main__':
    unittest.main()
