"""Acceptance must require physical timer wake and all actual preview captures."""
import copy
import unittest
import dev_pipeline as pipeline


def evidence():
    cases = []
    for i in range(7):
        cycle = dict(run=101+i, state=3, returnMode=3, timerArmed=True, timerWake=True,
                     usbPower=True, returnedHomeRun=101+i, wakeCaptureRun=101+i, wakeSkipped=False)
        cases.append(dict(index=i, home=['quiet','daily','study','vocabulary','quiet','daily','study'][i],
                          sleep=['quiet','daily','study','quiet','quiet','daily','study'][i], sleepWakeIndicator=i<4,
                          run=101+i, pixelDifferences=[0, 0, 0], settingsRetained=True,
                          wakePixels=150, cycle=cycle))
    captures = [dict(name=f'actual-{i}-{name}', run=101+i, width=480, height=800, sha256='a'*64)
                for i in range(7) for name in ('home', 'home-preview', 'sleep', 'sleep-preview', 'returned-home', 'returned-home-preview', 'wake')]
    return dict(device='X4', run='100', x4SleepValidation=dict(passed=True, restored=True, cases=cases, captures=captures))


class X4SleepEvidenceTest(unittest.TestCase):
    def test_complete_actual_evidence_passes(self):
        pipeline.check_x4_sleep(evidence())

    def test_timer_request_restart_and_missing_power_cannot_pass(self):
        for field, value in [('timerWake', False), ('timerArmed', False), ('usbPower', False),
                             ('returnMode', 0), ('returnedHomeRun', 99), ('state', 4), ('run', 99), ('wakeSkipped', True)]:
            record = evidence()
            record['x4SleepValidation']['cases'][0]['cycle'][field] = value
            with self.subTest(field=field), self.assertRaises(RuntimeError):
                pipeline.check_x4_sleep(record)

    def test_wrong_pixels_missing_frames_unrestored_or_wrong_cue_cannot_pass(self):
        good = evidence()
        changes = [lambda d: d['cases'][2].update(home='quiet'), lambda d: d['cases'][4].update(sleepWakeIndicator=True),
                   lambda d: d.update(restored=False), lambda d: d['cases'].pop(),
                   lambda d: d['captures'].pop(), lambda d: d['cases'][0].update(pixelDifferences=[0, 1]),
                   lambda d: d['cases'][4].update(wakePixels=0),
                   lambda d: d['captures'][0].update(run=999), lambda d: d['captures'][0].update(width=528)]
        for change in changes:
            record = copy.deepcopy(good)
            change(record['x4SleepValidation'])
            with self.assertRaises(RuntimeError):
                pipeline.check_x4_sleep(record)

if __name__ == '__main__':
    unittest.main()
