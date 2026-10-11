import json
import hashlib
import plistlib
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import dev_pipeline as pipeline


class PipelineTest(unittest.TestCase):
    def evidence(self):
        return {'passed': True, 'initialConnection': True, 'run': '123', 'version': 'x-dev-y',
                'platform': 'macOS', 'standbyWaitSeconds': 90, 'cycle': {
                    'run': 123, 'state': 3, 'frameReleased': True, 'frameBytes': 52272,
                    'beforeFree': 60000, 'afterFree': 112272, 'statsValid': True,
                    'gate': 'open', 'opened': 1, 'minFree': 61000, 'minBlock': 53000,
                    'fromWifi': True, 'returnMode': 2, 'close': 'app-wifi',
                    'lightSleeps': 12, 'lightSleepMs': 50, 'standbyMs': 60000}}

    def validate(self, evidence):
        return pipeline.hardware_evidence(pipeline.MARKER + json.dumps(evidence),
                                          123, 'x-dev-y', True, 'macOS')

    def test_wifi_setup_requires_verified_rollback(self):
        record = self.evidence()
        def validate():
            return pipeline.hardware_evidence(pipeline.MARKER + json.dumps(record),
                                              123, 'x-dev-y', False, 'macOS', wifi_setup=True)
        with self.assertRaises(RuntimeError):
            validate()
        record['wifiSetupRollback'] = 'passed'
        self.assertEqual(validate()['wifiSetupRollback'], 'passed')

    def test_interface_requires_every_case_and_recovery(self):
        record = self.evidence()
        record['transferFeedbackRecovery'] = 'passed'
        record['interfaceCases'] = [{'id': name, 'state': 'passed'} for name in sorted(pipeline.INTERFACE_CASES)]
        def validate(value):
            return pipeline.hardware_evidence(pipeline.MARKER + json.dumps(value),
                                              123, 'x-dev-y', True, 'macOS', True)
        self.assertTrue(validate(record)['passed'])
        for state in ['failed', 'not-run', 'not-applicable', 'running']:
            broken = json.loads(json.dumps(record))
            broken['interfaceCases'][0]['state'] = state
            with self.assertRaises(RuntimeError): validate(broken)
        record['interfaceCases'].pop()
        with self.assertRaises(RuntimeError): validate(record)

    def test_screen_evidence_rejects_stale_missing_and_unrestored_captures(self):
        record = self.evidence()
        names = ['01-home-reading', '02-home-word', '03-home-reading-restored',
                 '04-home-weather-bottom', '05-home-weather-top', '06-brief-weather',
                 '07-brief-today', '08-brief-weather-restored', '09-card-image-a',
                 '10-card-image-b', '11-card-image-restored', '12-reader-body', '13-actual-sleep']
        record['screenValidation'] = {'passed': True, 'restored': True, 'staleCaptureRejected': True,
            'cardsRestored': True, 'readingPreserved': True, 'pixelDifferences': {'cardImage': 4000, 'cardRestored': 0},
            'sleepCycle': {'run': 136, 'state': 3, 'returnMode': 1, 'timerArmed': True, 'timerWake': True},
            'captures': [{'name': name, 'run': 124 + i, 'width': 528, 'height': 792,
                          'sha256': 'a' * 64} for i, name in enumerate(names)]}
        def validate(value):
            return pipeline.hardware_evidence(pipeline.MARKER + json.dumps(value),
                                              123, 'x-dev-y', False, 'macOS', screens=True)
        self.assertTrue(validate(record)['passed'])
        for key in ['passed', 'restored', 'staleCaptureRejected', 'cardsRestored', 'readingPreserved']:
            broken = json.loads(json.dumps(record))
            broken['screenValidation'][key] = False
            with self.assertRaises(RuntimeError): validate(broken)
        broken = json.loads(json.dumps(record))
        broken['screenValidation']['captures'][0]['run'] = 123
        with self.assertRaises(RuntimeError): validate(broken)
        for key, value in [('run', 135), ('state', 4), ('returnMode', 0), ('timerArmed', False), ('timerWake', False)]:
            broken = json.loads(json.dumps(record))
            broken['screenValidation']['sleepCycle'][key] = value
            with self.assertRaises(RuntimeError): validate(broken)
        for key, value in [('cardImage', 0), ('cardRestored', 42)]:
            broken = json.loads(json.dumps(record))
            broken['screenValidation']['pixelDifferences'][key] = value
            with self.assertRaises(RuntimeError): validate(broken)
        record['screenValidation']['captures'].pop()
        with self.assertRaises(RuntimeError): validate(record)

    def test_screen_attachment_export_checks_names_and_exact_bytes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            payload = b'captured-bmp-fixture'
            captures = [{'name': '01-home-reading', 'sha256': hashlib.sha256(payload).hexdigest()}]
            items = []
            for suffix in ['bmp', 'png']:
                (root / ('UUID.' + suffix)).write_bytes(payload)
                items.append({'suggestedHumanReadableName': '01-home-reading_0_ACTIVITY.' + suffix,
                              'exportedFileName': 'UUID.' + suffix})
            def manifest():
                (root / 'manifest.json').write_text(json.dumps([{'attachments': items}]))
            manifest()
            pipeline.collect_screen_artifacts(root, captures)
            self.assertEqual((root / '01-home-reading.bmp').read_bytes(), payload)
            (root / 'UUID.bmp').write_bytes(b'corruption')
            with self.assertRaises(RuntimeError): pipeline.collect_screen_artifacts(root, captures)
            (root / 'UUID.bmp').write_bytes(payload)
            items.append(items[0]); manifest()
            with self.assertRaises(RuntimeError): pipeline.collect_screen_artifacts(root, captures)
            items.clear(); manifest()
            with self.assertRaises(RuntimeError): pipeline.collect_screen_artifacts(root, captures)

    def test_matching_real_hardware_evidence(self):
        self.assertTrue(self.validate(self.evidence())['passed'])

    def test_wrong_run_version_platform_and_failure_are_rejected(self):
        for field, value in [('run', 'old'), ('version', 'other'), ('platform', 'simulator'),
                             ('passed', False), ('initialConnection', False)]:
            with self.subTest(field=field):
                record = self.evidence()
                record[field] = value
                with self.assertRaises(RuntimeError):
                    self.validate(record)

    def test_sleep_metrics_are_required_not_just_test_pass(self):
        for field, value in [('run', 122), ('minFree', 50000), ('close', 'timeout'),
                             ('lightSleepMs', 0), ('fromWifi', False)]:
            with self.subTest(field=field):
                record = self.evidence()
                record['cycle'][field] = value
                with self.assertRaises((RuntimeError, pipeline.pocket_put.ProtocolError)):
                    self.validate(record)

    def test_short_or_missing_idle_interval_is_not_long_standby_evidence(self):
        for duration in [0, 59999]:
            record = self.evidence()
            record['cycle']['standbyMs'] = duration
            with self.assertRaisesRegex(RuntimeError, 'first minute'):
                self.validate(record)
        record = self.evidence()
        del record['standbyWaitSeconds']
        with self.assertRaisesRegex(RuntimeError, 'first minute'):
            self.validate(record)

    def test_skipped_missing_duplicate_and_malformed_evidence_are_rejected(self):
        good = pipeline.MARKER + json.dumps(self.evidence())
        for log in ('Test skipped; TEST SUCCEEDED', good + '\n' + good,
                    pipeline.MARKER + 'not-json'):
            with self.assertRaises((RuntimeError, ValueError)):
                pipeline.hardware_evidence(log, 123, 'x-dev-y', True, 'macOS')

    def test_failed_stage_persists_and_stops_next_mutation(self):
        with tempfile.TemporaryDirectory() as directory:
            job = pipeline.Pipeline(Path(directory) / 'run')
            with self.assertRaises(RuntimeError):
                job.command('failed-build', [sys.executable, '-c', 'raise SystemExit(7)'])
                job.command('must-not-flash', [sys.executable, '-c', 'raise SystemExit(0)'])
            report = json.loads((job.output / 'report.json').read_text())
            self.assertFalse(report['passed'])
            self.assertEqual([s['name'] for s in report['stages']], ['failed-build'])
            self.assertEqual(report['stages'][0]['state'], 'failed')
            self.assertIn('exit 7', report['stages'][0]['error'])

    def test_lock_excludes_another_process(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'lock'
            with pipeline.exclusive(path):
                code = ('from pathlib import Path; import dev_pipeline as p; '
                        f'lock=p.exclusive(Path({str(path)!r})); lock.__enter__()')
                result = subprocess.run([sys.executable, '-c', code], cwd=Path(__file__).parent,
                                        capture_output=True)
                self.assertNotEqual(result.returncode, 0)
            with pipeline.exclusive(path):
                pass

    def test_timeout_is_a_recorded_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            job = pipeline.Pipeline(Path(directory) / 'run')
            with self.assertRaises(subprocess.TimeoutExpired):
                job.command('hang', [sys.executable, '-c', 'import time; time.sleep(5)'], timeout=0.05)
            report = json.loads((job.output / 'report.json').read_text())
            self.assertEqual(report['stages'][0]['state'], 'failed')
            self.assertFalse(report['passed'])

    def test_running_normal_app_blocks_hardware_before_mutation(self):
        with tempfile.TemporaryDirectory() as directory:
            bundle = Path(directory) / 'Pocket Daily.app'
            (bundle / 'Contents').mkdir(parents=True)
            (bundle / 'Contents/Info.plist').write_bytes(plistlib.dumps(
                {'CFBundleIdentifier': 'bound.serendipity.pocket.daily'}))
            with patch.object(pipeline.subprocess, 'check_output',
                              return_value=f' 42 {bundle}/Contents/MacOS/Pocket\n'):
                with self.assertRaisesRegex(RuntimeError, 'already running'):
                    pipeline.require_companion_stopped()
            with patch.object(pipeline.subprocess, 'check_output', return_value=' 43 /bin/other\n'):
                pipeline.require_companion_stopped()

    def test_preflight_rejects_identity_mode_and_production_version(self):
        good = {'deviceID': 'expected', 'mode': 'STA', 'version': 'v-dev-test'}
        with patch.object(pipeline.cycle, 'get_json', return_value=good):
            self.assertEqual(pipeline.reader_status('192.168.1.2', 'expected'), good)
        for key, value in [('deviceID', 'other'), ('mode', 'AP'), ('version', '1.0.0')]:
            with patch.object(pipeline.cycle, 'get_json', return_value={**good, key: value}):
                with self.assertRaises((RuntimeError, pipeline.pocket_put.ProtocolError)):
                    pipeline.reader_status('192.168.1.2', 'expected')

    def test_host_rejects_public_loopback_and_non_address(self):
        for address in ['8.8.8.8', '127.0.0.1', '0.0.0.0', 'http://192.168.1.2/path']:
            with self.assertRaises(ValueError):
                pipeline.private_host(address)
        self.assertEqual(pipeline.private_host('192.168.1.2'), '192.168.1.2')


if __name__ == '__main__':
    unittest.main()
