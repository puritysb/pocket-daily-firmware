import json
import hashlib
import plistlib
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

import dev_pipeline as pipeline


class PipelineTest(unittest.TestCase):
    def test_published_epub_manifest_matches_the_committed_test_file(self):
        payload = (pipeline.ROOT / 'test/reading_progress/fixtures/frankenstein-se.epub').read_bytes()
        self.assertEqual(len(payload), pipeline.REAL_EPUB_BYTES)
        self.assertEqual(hashlib.sha256(payload).hexdigest(), pipeline.REAL_EPUB_SHA256)

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

    def test_continuity_requires_both_acceptance_directions_and_restoration(self):
        record = self.evidence()
        record['continuityValidation'] = dict(passed=True, restored=True,
            captures=[dict(name=name, run=143, sha256='a' * 64) for name in
                ['continuity-seed', 'continuity-app-to-x3-0', 'continuity-x3-to-app-0',
                 'continuity-app-to-x3-1', 'continuity-x3-to-app-1']],
            cases=[dict(direction='bidirectional', passed=True, round=index,
                        noMovementBeforeAcceptance=True, acceptedCharacterVisible=True, navigatedFromAnotherChapter=True, backwardOffer=backward)
                   for index, backward in enumerate([False, True])])
        def validate(value):
            return pipeline.hardware_evidence(pipeline.MARKER + json.dumps(value),
                123, 'x-dev-y', True, 'macOS', continuity=True)
        self.assertTrue(validate(record)['passed'])
        for key, value in [('passed', False), ('restored', False), ('captures', []), ('cases', [])]:
            broken = json.loads(json.dumps(record))
            broken['continuityValidation'][key] = value
            with self.assertRaises(RuntimeError): validate(broken)
        for key, value in [('passed', False), ('direction', 'one-way'), ('noMovementBeforeAcceptance', False), ('backwardOffer', True), ('acceptedCharacterVisible', False), ('navigatedFromAnotherChapter', False), ('round', 1)]:
            broken = json.loads(json.dumps(record))
            broken['continuityValidation']['cases'][0][key] = value
            with self.assertRaises(RuntimeError): validate(broken)

        record['continuityValidation']['epub'] = dict(kind='published-book',
            sha256='d09a9d94bd1076edd07a9204989ed2917bef73ad28cbd855d758641fb2e5eeb0',
            bytes=226309, roundTripBytesMatch=True, libraryReimportMatches=True)
        def validate_real(value):
            return pipeline.hardware_evidence(pipeline.MARKER + json.dumps(value),
                123, 'x-dev-y', True, 'macOS', continuity=True, real_epub=True)
        self.assertTrue(validate_real(record)['passed'])
        for key, value in [('kind', 'generated'), ('sha256', 'a' * 64), ('bytes', 1),
                           ('roundTripBytesMatch', False), ('libraryReimportMatches', False)]:
            broken = json.loads(json.dumps(record))
            broken['continuityValidation']['epub'][key] = value
            with self.assertRaises(RuntimeError): validate_real(broken)

        for key, value in [('name', 'old-capture'), ('run', 999), ('sha256', '')]:
            broken = json.loads(json.dumps(record))
            broken['continuityValidation']['captures'][0][key] = value
            with self.assertRaises(RuntimeError): validate(broken)

    def test_network_fault_requires_real_outage_resume_and_single_publication(self):
        record = self.evidence()
        record['networkFaults'] = [dict(run=123, state=4, point=point, sawOffline=True,
            outageMs=5200, publications=1, passed=True, resumed=65536) for point in ['receiving', 'published']]
        def validate(value):
            return pipeline.hardware_evidence(pipeline.MARKER + json.dumps(value),
                123, 'x-dev-y', True, 'macOS', network_faults=True)
        self.assertTrue(validate(record)['passed'])
        for key, value in [('run', 124), ('state', 2), ('sawOffline', False),
                           ('outageMs', 4999), ('publications', 2), ('passed', False), ('resumed', 0)]:
            broken = json.loads(json.dumps(record))
            broken['networkFaults'][0][key] = value
            with self.assertRaises(RuntimeError): validate(broken)
        record['networkFaults'].pop()
        with self.assertRaises(RuntimeError): validate(record)

    def test_delayed_advertising_requires_real_scan_before_connection(self):
        record = self.evidence()
        record['cycle'].update(advertisingDelayMs=106000, advertisingStartedMs=106250)
        record['wakeObservations'] = [
            {'event': name, 'seconds': seconds} for name, seconds in [
                ('connecting', 0.05), ('connectionReset', 8), ('pendingConnectionCancelled', 8.005),
                ('discoveryStarted', 8.01), ('knownAdvertisement', 12.4), ('reconnectRequested', 12.41),
                ('connected', 12.6),
                ('authenticated', 13.1), ('commandSent', 13.2), ('accepted', 13.4)]]
        def validate(value):
            return pipeline.hardware_evidence(pipeline.MARKER + json.dumps(value),
                123, 'x-dev-y', True, 'macOS', delayed_advertising=True)
        self.assertTrue(validate(record)['passed'])
        for missing in ['connecting', 'connectionReset', 'pendingConnectionCancelled', 'discoveryStarted',
                        'knownAdvertisement', 'reconnectRequested', 'connected', 'authenticated', 'commandSent', 'accepted']:
            broken = json.loads(json.dumps(record))
            broken['wakeObservations'] = [v for v in broken['wakeObservations'] if v['event'] != missing]
            with self.assertRaises(RuntimeError): validate(broken)
        for field, value in [('advertisingDelayMs', 0), ('advertisingStartedMs', 1)]:
            broken = json.loads(json.dumps(record)); broken['cycle'][field] = value
            with self.assertRaises(RuntimeError): validate(broken)
        for event, seconds in [('discoveryStarted', 0.1), ('connected', 21), ('connected', 7), ('knownAdvertisement', 4), ('accepted', 21)]:
            broken = json.loads(json.dumps(record))
            next(v for v in broken['wakeObservations'] if v['event'] == event)['seconds'] = seconds
            with self.assertRaises(RuntimeError): validate(broken)

    def test_control_cannot_be_counted_as_rediscovery_recovery(self):
        record = self.evidence()
        record['wakeControl'] = True
        record['cycle'].update(advertisingDelayMs=106000, advertisingStartedMs=106250)
        record['wakeObservations'] = [{'event': name, 'seconds': seconds} for name, seconds in [
            ('connecting', 0.1), ('connected', 15), ('authenticated', 15.2),
            ('commandSent', 15.3), ('accepted', 15.4)]]
        def validate(control):
            return pipeline.hardware_evidence(pipeline.MARKER + json.dumps(record),
                123, 'x-dev-y', True, 'macOS', delayed_advertising=True, wake_control=control)
        self.assertTrue(validate(True)['passed'])
        with self.assertRaises(RuntimeError): validate(False)
        record['wakeObservations'].append({'event': 'discoveryStarted', 'seconds': 8})
        with self.assertRaises(RuntimeError): validate(True)

    def test_ordinary_standby_rejects_leaked_advertising_fault(self):
        record = self.evidence()
        record['cycle']['advertisingDelayMs'] = 106000
        with self.assertRaises(RuntimeError): self.validate(record)
        record['cycle']['advertisingDelayMs'] = 0
        self.assertTrue(self.validate(record)['passed'])

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
                 '10-card-image-b', '11-card-image-restored', '12-reader-body', '13-actual-sleep', '14-wake-transition']
        record['screenValidation'] = {'passed': True, 'restored': True, 'staleCaptureRejected': True,
            'cardsRestored': True, 'readingPreserved': True, 'preferencesRestored': True, 'pixelDifferences': {'cardImage': 4000, 'cardRestored': 0, 'sleepContentRetained': 0, 'wakeBodyRetained': 0, 'wakeTransition': 500},
            'sleepCycle': {'run': 136, 'state': 3, 'returnMode': 1, 'timerArmed': True, 'timerWake': True, 'wakeCaptureRun': 136, 'wakeSkipped': False, 'wakeRefreshMs': 500},
            'captures': [{'name': name, 'run': 123 + min(i + 1, 13), 'width': 528, 'height': 792,
                          'sha256': 'a' * 64} for i, name in enumerate(names)]}
        def validate(value):
            return pipeline.hardware_evidence(pipeline.MARKER + json.dumps(value),
                                              123, 'x-dev-y', False, 'macOS', screens=True)
        self.assertTrue(validate(record)['passed'])
        for key in ['passed', 'restored', 'staleCaptureRejected', 'cardsRestored', 'readingPreserved', 'preferencesRestored']:
            broken = json.loads(json.dumps(record))
            broken['screenValidation'][key] = False
            with self.assertRaises(RuntimeError): validate(broken)
        broken = json.loads(json.dumps(record))
        broken['screenValidation']['captures'][0]['run'] = 123
        with self.assertRaises(RuntimeError): validate(broken)
        for key, value in [('run', 135), ('state', 4), ('returnMode', 0), ('timerArmed', False), ('timerWake', False), ('wakeCaptureRun', 135), ('wakeSkipped', True), ('wakeRefreshMs', 0), ('wakeRefreshMs', 1501)]:
            broken = json.loads(json.dumps(record))
            broken['screenValidation']['sleepCycle'][key] = value
            with self.assertRaises(RuntimeError): validate(broken)
        for key, value in [('cardImage', 0), ('cardRestored', 42), ('sleepContentRetained', 1), ('sleepContentRetained', None), ('wakeBodyRetained', 1), ('wakeTransition', 0)]:
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

    def test_long_standby_requires_matching_wait_and_elapsed_evidence(self):
        record = self.evidence()
        record['idleAppTrafficSuspended'] = True
        record['standbyWaitSeconds'] = 1800
        record['cycle']['standbyMs'] = 1780000
        record['cycle']['standbyLimitMs'] = 1890000
        log = pipeline.MARKER + json.dumps(record)
        pipeline.hardware_evidence(log, int(record['run']), record['version'], True, 'macOS', standby_seconds=1800)
        for field, value in [('standbyWaitSeconds', 90), ('idleAppTrafficSuspended', False)]:
            invalid = {**record, field: value}
            with self.assertRaises(RuntimeError):
                pipeline.hardware_evidence(pipeline.MARKER + json.dumps(invalid), int(record['run']),
                                          record['version'], True, 'macOS', standby_seconds=1800)
        record['cycle']['standbyLimitMs'] = 180000
        with self.assertRaises(RuntimeError):
            pipeline.hardware_evidence(pipeline.MARKER + json.dumps(record), int(record['run']),
                                      record['version'], True, 'macOS', standby_seconds=1800)
        record['cycle']['standbyLimitMs'] = 1890000
        record['cycle']['standbyMs'] = 90000
        with self.assertRaises(RuntimeError):
            pipeline.hardware_evidence(pipeline.MARKER + json.dumps(record), int(record['run']), record['version'], True,
                                       'macOS', standby_seconds=1800)

    def test_battery_only_rejects_charging_missing_and_protected_battery(self):
        record = self.evidence()
        record['batteryOnlyRequested'] = True
        record['cycle'].update(chargingAtCheckpoint=False, batteryPercent=26)
        def validate():
            return pipeline.hardware_evidence(pipeline.MARKER + json.dumps(record),
                                              123, 'x-dev-y', True, 'macOS', battery_only=True)
        self.assertTrue(validate()['passed'])
        for field, value in [('chargingAtCheckpoint', True), ('chargingAtCheckpoint', None),
                             ('batteryPercent', 10), ('batteryPercent', 0), ('batteryPercent', None)]:
            before = record['cycle'][field]
            record['cycle'][field] = value
            with self.assertRaisesRegex(RuntimeError, 'battery-only'): validate()
            record['cycle'][field] = before
        del record['batteryOnlyRequested']
        with self.assertRaisesRegex(RuntimeError, 'battery-only'): validate()

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

    def test_discovery_evidence_rejects_untrusted_or_stale_results(self):
        good = {'passed': True, 'run': '42', 'platform': 'macOS', 'identityVerified': True,
                'staleHintUsed': True, 'host': '192.168.2.3', 'port': 80, 'version': 'v-dev-test'}
        def validate(record):
            return pipeline.discovery_evidence(pipeline.DISCOVERY_MARKER + json.dumps(record), 42)
        self.assertEqual(validate(good), good)
        for key, value in [('passed', False), ('run', '41'), ('platform', 'simulator'),
                           ('identityVerified', False), ('staleHintUsed', False), ('port', 8080),
                           ('host', '8.8.8.8'), ('host', '127.0.0.1'), ('version', '1.0')]:
            with self.subTest(key=key, value=value), self.assertRaises((RuntimeError, ValueError)):
                validate({**good, key: value})
        log = pipeline.DISCOVERY_MARKER + json.dumps(good)
        for invalid in ['', log + '\n' + log, pipeline.DISCOVERY_MARKER + 'invalid']:
            with self.assertRaises((RuntimeError, ValueError)):
                pipeline.discovery_evidence(invalid, 42)

    def test_stale_or_reassigned_address_recovers_only_the_enrolled_reader(self):
        good = {'deviceID': 'expected', 'mode': 'STA', 'version': 'v-dev-test'}
        for initial in [OSError('offline'), {**good, 'deviceID': 'different'}, {}]:
            with self.subTest(initial=initial), tempfile.TemporaryDirectory() as tmp:
                path = Path(tmp) / 'reader.json'
                config = {'host': '192.168.1.2', 'deviceID': 'expected', 'extra': 'preserved'}
                pipeline.save(path, config)
                discovered = {'host': '192.168.2.3', 'version': good['version']}
                discover = Mock(return_value=discovered)
                with patch.object(pipeline.cycle, 'get_json', side_effect=[initial, good]) as probe:
                    host, status, evidence = pipeline.resolve_reader(config, path, discover)
                self.assertEqual(host, discovered['host'])
                self.assertEqual(status, good)
                self.assertTrue(evidence['addressChanged'])
                self.assertEqual(probe.call_args_list[-1].args[0], host)
                self.assertEqual(config, json.loads(path.read_text()))
                self.assertEqual(config['deviceID'], 'expected')
                self.assertEqual(config['extra'], 'preserved')
                discover.assert_called_once_with()

    def test_failed_discovery_never_reenrolls_or_rewrites_config(self):
        good = {'deviceID': 'expected', 'mode': 'STA', 'version': 'v-dev-test'}
        failures = [OSError('offline'), {**good, 'deviceID': 'different'}, {},
                    {**good, 'version': 'v-dev-other'}, {**good, 'mode': 'AP'}]
        for response in failures:
            with self.subTest(response=response), tempfile.TemporaryDirectory() as tmp:
                path = Path(tmp) / 'reader.json'
                original = {'host': '192.168.1.2', 'deviceID': 'expected'}
                config = dict(original)
                pipeline.save(path, original)
                before = path.read_bytes()
                discover = Mock(return_value={'host': '192.168.2.3', 'version': good['version']})
                with patch.object(pipeline.cycle, 'get_json', side_effect=[OSError(), response]):
                    with self.assertRaises((OSError, RuntimeError, pipeline.pocket_put.ProtocolError)):
                        pipeline.resolve_reader(config, path, discover)
                self.assertEqual(path.read_bytes(), before)
                self.assertEqual(config, original)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'reader.json'
            pipeline.save(path, original)
            for error in [RuntimeError('missing or duplicate evidence'), TimeoutError('app timeout')]:
                with patch.object(pipeline.cycle, 'get_json', side_effect=OSError()):
                    with self.assertRaises((RuntimeError, TimeoutError)):
                        pipeline.resolve_reader(dict(original), path, Mock(side_effect=error))
                self.assertEqual(json.loads(path.read_text()), original)

    def test_pipeline_continues_hardware_suite_at_verified_new_address(self):
        from types import SimpleNamespace
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            config = {'host': '192.168.1.2', 'deviceID': 'expected'}
            config_path = root / 'reader.json'
            pipeline.save(config_path, config)
            args = SimpleNamespace(app=root, config=config_path, standby=False, checks=False,
                                   build_firmware=None, firmware=None, ios_simulator=None,
                                   ios_device=None, cycles=1)
            binary = root / '.build/dev-pipeline-mac/Build/Products/Debug/Pocket.app/Contents/MacOS/Pocket'
            binary.parent.mkdir(parents=True)
            binary.write_bytes(b'test app')
            job = pipeline.Pipeline(root / 'run')
            hardware_hosts = []
            def command(name, argv, **kwargs):
                log = job.output / (name + '.log')
                if name == 'macOS-hardware-1':
                    env = kwargs['env']
                    hardware_hosts.append(env['TEST_RUNNER_POCKET_HARDWARE_HOST'])
                    record = {'passed': True, 'initialConnection': True, 'platform': 'macOS',
                              'run': env['TEST_RUNNER_POCKET_HARDWARE_RUN'], 'version': 'v-dev-test'}
                    log.write_text(pipeline.MARKER + json.dumps(record))
                return log
            good = {'deviceID': 'expected', 'mode': 'STA', 'version': 'v-dev-test'}
            with patch.object(pipeline, 'source_state', return_value={}), \
                 patch.object(pipeline, 'require_companion_stopped'), \
                 patch.object(job, 'command', side_effect=command), \
                 patch.object(pipeline, 'discover_with_app', return_value={
                     'host': '192.168.2.3', 'version': 'v-dev-test'}), \
                 patch.object(pipeline.cycle, 'get_json', side_effect=[OSError(), good]):
                pipeline.run(args, config, job)
            self.assertEqual(hardware_hosts, ['192.168.2.3'])
            self.assertTrue(job.report['passed'])
            self.assertEqual(job.report['discovery']['method'], 'app-discovery')
            self.assertEqual(json.loads(config_path.read_text())['host'], hardware_hosts[0])

    def test_reachable_saved_address_does_not_launch_discovery(self):
        good = {'deviceID': 'expected', 'mode': 'STA', 'version': 'v-dev-test'}
        discover = Mock(side_effect=AssertionError('must not discover'))
        config = {'host': '192.168.1.2', 'deviceID': 'expected'}
        with patch.object(pipeline.cycle, 'get_json', return_value=good):
            host, _, evidence = pipeline.resolve_reader(config, None, discover)
        self.assertEqual(host, config['host'])
        self.assertEqual(evidence['method'], 'saved-address')
        discover.assert_not_called()

    def test_host_rejects_public_loopback_and_non_address(self):
        for address in ['8.8.8.8', '127.0.0.1', '0.0.0.0', 'http://192.168.1.2/path']:
            with self.assertRaises(ValueError):
                pipeline.private_host(address)
        self.assertEqual(pipeline.private_host('192.168.1.2'), '192.168.1.2')


if __name__ == '__main__':
    unittest.main()
