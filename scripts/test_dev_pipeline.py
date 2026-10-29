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
    def test_x4_settings_requires_all_orientations_pixels_and_reboot(self):
        import copy
        evidence = {'device': 'X4', 'x4SettingsValidation': {
            'passed': True, 'restored': True,
            'cases': [dict(orientation=i, preferencesReadBack=True, profileReadBack=True,
                           rebootPersisted=True, homeRetainedDifference=0, cardPixelDifference=0, readingFrameChanged=True, cardPersisted=True) for i in range(4)],
            'captures': [dict(name=f'x4-{surface}-{i}', width=480, height=800, sha256='a' * 64)
                         for i in range(4) for surface in ('home', 'home-retained', 'brief', 'card', 'reading')]
                        + [dict(name=f'x4-typography-{field}', width=480, height=800, sha256='a' * 64)
                           for field in ('fontSize', 'lineSpacing', 'screenMargin')],
            'typography': [dict(field=field, bodyPixelDifference=501, rebootPersisted=True)
                           for field in ('fontSize', 'lineSpacing', 'screenMargin')]}}
        pipeline.check_x4_settings(evidence)
        portrait_only = copy.deepcopy(evidence)
        portrait_only['x4SettingsValidation']['cases'] = portrait_only['x4SettingsValidation']['cases'][:1]
        captures = portrait_only['x4SettingsValidation']['captures']
        portrait_only['x4SettingsValidation']['captures'] = captures[:5] + captures[-3:]
        pipeline.check_x4_settings(portrait_only, typography_only=True)
        with self.assertRaises(RuntimeError): pipeline.check_x4_settings(portrait_only)
        with self.assertRaises(RuntimeError): pipeline.check_x4_settings(evidence, typography_only=True)
        for key, value in [('rebootPersisted', False), ('homeRetainedDifference', 1), ('readingFrameChanged', False), ('cardPersisted', False), ('cardPixelDifference', 1),
                           ('preferencesReadBack', False), ('profileReadBack', False), ('orientation', 9)]:
            bad = copy.deepcopy(evidence)
            bad['x4SettingsValidation']['cases'][0][key] = value
            with self.assertRaises(RuntimeError): pipeline.check_x4_settings(bad)
        for key, value in [('width', 528), ('height', 792), ('sha256', ''), ('name', 'stale')]:
            bad = copy.deepcopy(evidence)
            bad['x4SettingsValidation']['captures'][0][key] = value
            with self.assertRaises(RuntimeError): pipeline.check_x4_settings(bad)
        for key, value in [('passed', False), ('restored', False), ('captures', []), ('cases', []), ('typography', [])]:
            bad = copy.deepcopy(evidence)
            bad['x4SettingsValidation'][key] = value
            with self.assertRaises(RuntimeError): pipeline.check_x4_settings(bad)
        for key, value in [('field', 'ignored'), ('bodyPixelDifference', 500), ('rebootPersisted', False)]:
            bad = copy.deepcopy(evidence)
            bad['x4SettingsValidation']['typography'][0][key] = value
            with self.assertRaises(RuntimeError): pipeline.check_x4_settings(bad)

    def test_x4_starters_reject_missing_cards_presets_reboots_and_stale_frames(self):
        import copy
        groups = [dict(id=name, cards=[name + str(i) for i in range(3)], rebootPersisted=True,
                       retainedPixelDifference=0) for name in pipeline.STARTER_COLLECTIONS]
        cards = [dict(collection=g['id'], card=card, pixelDifference=0, readBack=True, applySeconds=2.5)
                 for g in groups for card in g['cards'][1:] + g['cards'][:1]]
        presets = [dict(scope=scope, id=name, profileReadBack=True, unrelatedSettingsPreserved=True,
                        rebootPersisted=True, cardsPreserved=True, retainedBodyPixelDifference=0,
                        previewUsesReaderData=True, previewPixelDifference=0, retainedPreviewPixelDifference=0, changeFromPrevious=1000)
                   for scope, name in pipeline.STARTER_PRESETS]
        names = []
        for group in groups:
            names += ['starter-card-' + c for c in group['cards'][1:] + group['cards'][:1]]
            names += ['starter-retained-' + group['id']]
        names += [f'preset-{scope.lower()}-{name}{suffix}' for scope, name in pipeline.STARTER_PRESETS for suffix in ('', '-retained')]
        evidence = dict(device='X4', x4StartersValidation=dict(passed=True, restored=True, cards=cards,
            collections=groups, presets=presets, layoutPreviewParity=True, layoutOnly=False,
            health=[dict(case=name, freeHeap=30000, uptime=60, crashReportAvailable=False) for name in
                    [c['card'] for c in cards] + [f'preset-{scope.lower()}-{name}' for scope, name in pipeline.STARTER_PRESETS]],
            captures=[dict(name=n, width=480, height=800, sha256='a'*64) for n in names]))
        pipeline.check_x4_starters(evidence)
        faults = {
            'cards': [('pixelDifference', 1), ('readBack', False), ('card', 'unrelated'), ('collection', 'unknown'), ('applySeconds', float('nan')), ('applySeconds', 0), ('applySeconds', True)],
            'collections': [('rebootPersisted', False), ('retainedPixelDifference', 1), ('cards', ['one'])],
            'presets': [('profileReadBack', False), ('unrelatedSettingsPreserved', False), ('cardsPreserved', False),
                        ('rebootPersisted', False), ('retainedBodyPixelDifference', 1), ('previewPixelDifference', 1), ('retainedPreviewPixelDifference', 1), ('previewUsesReaderData', False), ('id', 'invalid')],
            'captures': [('name', 'stale'), ('width', 528), ('sha256', 'z'*64)],
            'health': [('case', 'stale'), ('freeHeap', 16383), ('freeHeap', True), ('uptime', -1), ('crashReportAvailable', None)]}
        for section, mutations in faults.items():
            for key, value in mutations:
                bad = copy.deepcopy(evidence)
                bad['x4StartersValidation'][section][0][key] = value
                with self.subTest(section=section, key=key), self.assertRaises(RuntimeError):
                    pipeline.check_x4_starters(bad)
        for key in ('cards', 'collections', 'presets', 'captures', 'health', 'restored', 'passed', 'layoutPreviewParity'):
            bad = copy.deepcopy(evidence)
            bad['x4StartersValidation'][key] = [] if isinstance(bad['x4StartersValidation'][key], list) else False
            with self.subTest(missing=key), self.assertRaises(RuntimeError): pipeline.check_x4_starters(bad)
        bad = copy.deepcopy(evidence)
        bad['x4StartersValidation']['presets'][1]['changeFromPrevious'] = 0
        with self.assertRaises(RuntimeError): pipeline.check_x4_starters(bad)
        focused = copy.deepcopy(evidence)
        d = focused['x4StartersValidation']
        d.update(cards=[], collections=[], layoutOnly=True)
        d['captures'] = d['captures'][-14:]
        d['health'] = d['health'][-7:]
        pipeline.check_x4_starters(focused, layout_only=True)
        with self.assertRaises(RuntimeError): pipeline.check_x4_starters(focused)
        with self.assertRaises(RuntimeError): pipeline.check_x4_starters(evidence, layout_only=True)

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

    def test_ble_continuity_requires_every_leg_in_a_bonded_standby_window(self):
        record = self.evidence()
        cycles = []
        for index, (leg, armed, offers) in enumerate(pipeline.BLE_CONTINUITY_LEGS):
            cycle_run = 123 + 20 + 100 + index + 1
            cycle = dict(leg=leg, armed=armed, run=cycle_run, reader=dict(
                run=cycle_run, state=3, close='app-wifi', fromWifi=True, gate='open', offers=offers,
                lists=1 if armed else 0, connections=2 if armed else 1, minFree=70000, minBlock=60000))
            if armed: cycle['sent'] = offers
            cycles.append(cycle)
        record['continuityValidation'] = dict(passed=True, restored=True, transport='bluetooth',
            bluetoothCycles=cycles,
            captures=[dict(name=name, run=143, sha256='a' * 64) for name in
                ['continuity-seed', 'continuity-app-to-x3-0', 'continuity-x3-to-app-0',
                 'continuity-app-to-x3-1', 'continuity-x3-to-app-1']],
            cases=[dict(direction='bidirectional', passed=True, round=index, transport='bluetooth',
                        noMovementBeforeAcceptance=True, acceptedCharacterVisible=True,
                        navigatedFromAnotherChapter=True, backwardOffer=backward)
                   for index, backward in enumerate([False, True])])
        def validate(value):
            return pipeline.hardware_evidence(pipeline.MARKER + json.dumps(value),
                123, 'x-dev-y', True, 'macOS', continuity=True, ble_continuity=True)
        self.assertTrue(validate(record)['passed'])
        http = json.loads(json.dumps(record))
        http['continuityValidation']['transport'] = 'http'
        with self.assertRaises(RuntimeError): validate(http)
        missing = json.loads(json.dumps(record))
        missing['continuityValidation']['bluetoothCycles'].pop()
        with self.assertRaises(RuntimeError): validate(missing)
        case = json.loads(json.dumps(record))
        case['continuityValidation']['cases'][1]['transport'] = 'http'
        with self.assertRaises(RuntimeError): validate(case)
        # The unarmed control must neither list nor store an offer.
        for index, key, value in [(1, 'lists', 1), (1, 'offers', 1), (2, 'offers', 0), (2, 'lists', 0),
                                  (0, 'close', 'time-up'), (0, 'fromWifi', False), (0, 'gate', 'low-memory'),
                                  (0, 'state', 2), (0, 'run', 1), (0, 'minFree', 40000)]:
            broken = json.loads(json.dumps(record))
            broken['continuityValidation']['bluetoothCycles'][index]['reader'][key] = value
            with self.assertRaises(RuntimeError, msg=f'{index} {key}'): validate(broken)
        for index, key, value in [(2, 'sent', 0), (1, 'armed', True), (0, 'leg', 'app-to-x3-0'), (0, 'run', 7)]:
            broken = json.loads(json.dumps(record))
            broken['continuityValidation']['bluetoothCycles'][index][key] = value
            with self.assertRaises(RuntimeError, msg=f'{index} {key}'): validate(broken)

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

    def test_x3_only_rejects_other_models_before_hardware_operations(self):
        from types import SimpleNamespace
        for device in ['X4', None, 'unknown']:
            with self.subTest(device=device), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                config = {'host': '192.168.1.2', 'deviceID': 'expected'}
                args = SimpleNamespace(app=root, config=root / 'reader.json', x3_only=True)
                binary = root / '.build/dev-pipeline-mac/Build/Products/Debug/Pocket.app/Contents/MacOS/Pocket'
                binary.parent.mkdir(parents=True)
                binary.write_bytes(b'test app')
                job = pipeline.Pipeline(root / 'run')
                status = {'device': device, 'version': 'v-dev-test'}
                with patch.object(pipeline, 'source_state', return_value={}), \
                     patch.object(pipeline, 'require_companion_stopped'), \
                     patch.object(job, 'command') as command, \
                     patch.object(pipeline, 'resolve_reader', return_value=(config['host'], status, {})):
                    with self.assertRaisesRegex(RuntimeError, 'X3-only'):
                        pipeline.run(args, config, job)
                self.assertEqual([c.args[0] for c in command.call_args_list], ['app-project', 'mac-build'])

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




class InstallOnlyTest(unittest.TestCase):
    def args(self, root, **changes):
        from types import SimpleNamespace
        fields = dict(action='run', app=root, config=root / 'config.json', host=None,
                      firmware=root / 'input.bin', flash=True, install_only=True,
                      checks=False, build_firmware=None, ios_simulator=None, ios_device=None,
                      cycles=1, standby=False, standby_seconds=90,
                      cppcheck='build/cppcheck-source/cppcheck')
        fields.update(changes)
        return SimpleNamespace(**fields)

    def status(self, version='v-dev-old', **changes):
        return dict(deviceID='enrolled', device='X4', mode='STA', version=version, **changes)

    def execute(self, root, responses, *, command_error=None, companion_error=None, payload=None, identity='enrolled'):
        args = self.args(root)
        args.firmware.write_bytes(payload if payload is not None else b'CrossPoint version: v-dev-next\0')
        config = dict(host='192.168.1.2', deviceID=identity)
        job = pipeline.Pipeline(root / 'run')
        commands = []
        def command(name, argv, **kwargs):
            commands.append((name, list(argv)))
            self.assertEqual(name, 'upload-install-reboot', 'install-only must not run app/build/hardware commands')
            self.assertEqual(Path(argv[2]).read_bytes(), args.firmware.read_bytes())
            self.assertIn('--dev-flash', argv)
            def perform():
                if command_error:
                    raise command_error
                path = job.output / 'mock-upload.log'
                path.write_text('offline command double\n')
                return path
            return job.stage(name, perform)
        error = None
        with patch.object(pipeline, 'source_state', return_value={'offline': True}), \
             patch.object(pipeline, 'require_companion_stopped', side_effect=companion_error) as stopped, \
             patch.object(pipeline, 'discover_with_app', side_effect=AssertionError('no app discovery')) as discover, \
             patch.object(pipeline.cycle, 'get_json', side_effect=responses) as status, \
             patch.object(job, 'command', side_effect=command):
            try:
                pipeline.run(args, config, job)
            except Exception as caught:
                error = caught
        discover.assert_not_called()
        return args, job, commands, status.call_count, stopped.call_count, error

    def test_install_requires_explicit_run_flash_and_firmware(self):
        for change in [dict(action='configure'), dict(flash=False), dict(firmware=None)]:
            with self.subTest(change=change):
                self.assertIsNotNone(pipeline.install_only_argument_error(self.args(Path('/unused'), **change)))
        self.assertIsNone(pipeline.install_only_argument_error(self.args(Path('/unused'))))

    def test_all_scenarios_and_unused_options_conflict(self):
        names = ('host', 'build_firmware', 'checks', 'ios_simulator', 'ios_device',
                 'x4_recover_empty_revision', 'x4_recover_from', 'x4_sleep', 'x4_starters',
                 'x4_layout_only', 'x4_settings', 'x4_typography_only', 'x3_only', 'wifi_setup',
                 'real_epub', 'continuity_suite', 'ble_continuity', 'screen_suite',
                 'interface_suite', 'network_faults', 'standby', 'wake_control',
                 'delayed_advertising', 'battery_only')
        for name in names:
            with self.subTest(name=name):
                self.assertIsNotNone(pipeline.install_only_argument_error(self.args(Path('/unused'), **{name: True})))
        for change in [dict(cycles=0), dict(cycles=2), dict(standby_seconds=91), dict(cppcheck='/custom')]:
            with self.subTest(change=change):
                self.assertIsNotNone(pipeline.install_only_argument_error(self.args(Path('/unused'), **change)))

    def test_invalid_cli_rejected_before_lock_config_status_or_commands(self):
        argv_sets = [
            ['configure', '--install-only', '--flash', '--firmware', '/unused'],
            ['run', '--install-only', '--firmware', '/unused'],
            ['run', '--install-only', '--flash'],
        ] + [['run', '--install-only', '--flash', '--firmware', '/unused'] + flags for flags in [
            ['--checks'], ['--build-firmware', 'default'], ['--ios-device', 'offline'],
            ['--ios-simulator', 'offline'], ['--x4-settings'], ['--cycles', '2'],
            ['--screen-suite'], ['--continuity-suite']]]
        import contextlib, io
        for argv in argv_sets:
            with self.subTest(argv=argv), patch.object(sys, 'argv', ['dev_pipeline.py'] + argv), \
                 patch.object(pipeline, 'exclusive') as lock, \
                 patch.object(pipeline.cycle, 'get_json') as status, \
                 patch.object(pipeline, 'run') as run, \
                 patch.object(pipeline.os, 'umask'), \
                 patch.dict(pipeline.os.environ, {}, clear=True), \
                 contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as failure:
                    pipeline.main()
                self.assertEqual(failure.exception.code, 2)
                lock.assert_not_called(); status.assert_not_called(); run.assert_not_called()

    def test_run_entry_also_rejects_invalid_install_scope_before_io(self):
        args = self.args(Path('/unused'), flash=False)
        job = Mock()
        with self.assertRaises(ValueError), patch.object(pipeline, 'require_companion_stopped') as stopped:
            pipeline.run(args, {}, job)
        stopped.assert_not_called(); job.stage.assert_not_called()

    def test_install_success_freezes_bytes_checks_exact_version_and_runs_no_hardware(self):
        with tempfile.TemporaryDirectory() as tmp:
            args, job, commands, calls, stopped, error = self.execute(Path(tmp), [self.status(), self.status(), self.status('v-dev-next')])
            self.assertIsNone(error)
            self.assertEqual(len(commands), 1)
            self.assertEqual(calls, 3)
            self.assertEqual(stopped, 2)
            self.assertEqual((job.output / 'candidate.bin').read_bytes(), args.firmware.read_bytes())
            self.assertEqual(job.report['artifact']['sha256'], hashlib.sha256(args.firmware.read_bytes()).hexdigest())
            self.assertTrue(job.report['passed']); self.assertTrue(job.report['installationOnly'])
            self.assertFalse(job.report['acceptancePerformed']); self.assertEqual(job.report['hardware'], [])
            self.assertEqual(job.report['scope'], 'installation-only')
            self.assertNotIn('macApp', job.report)
            self.assertEqual(job.report['stages'][-1]['name'], 'installed-version')
            self.assertEqual(job.report['stages'][-1]['state'], 'passed')

    def test_already_installed_skips_flash_but_rechecks_exact_version(self):
        with tempfile.TemporaryDirectory() as tmp:
            _, job, commands, calls, _, error = self.execute(Path(tmp), [self.status('v-dev-next')] * 3)
            self.assertIsNone(error); self.assertEqual(commands, []); self.assertEqual(calls, 3)
            self.assertIn('flash skipped', job.report['installation'])
            self.assertTrue(job.report['passed']); self.assertFalse(job.report['acceptancePerformed'])

    def test_stale_or_wrong_initial_identity_mode_version_never_flashes_or_discovers(self):
        cases = [OSError('offline'), {**self.status(), 'deviceID': 'other'},
                 {**self.status(), 'mode': 'AP'}, {**self.status(), 'version': '0.1.0'}]
        for response in cases:
            with self.subTest(response=type(response).__name__), tempfile.TemporaryDirectory() as tmp:
                _, job, commands, calls, _, error = self.execute(Path(tmp), [response])
                self.assertIsNotNone(error); self.assertEqual(commands, []); self.assertEqual(calls, 1)
                self.assertFalse(job.report['passed']); self.assertFalse(job.report['acceptancePerformed'])

    def test_missing_enrollment_fails_before_reader_io(self):
        for identity in ['', None, 7]:
            with self.subTest(identity=identity), tempfile.TemporaryDirectory() as tmp:
                _, job, commands, calls, _, error = self.execute(Path(tmp), [], identity=identity)
                self.assertIsInstance(error, ValueError); self.assertEqual(commands, []); self.assertEqual(calls, 0)
                self.assertFalse(job.report['passed'])

    def test_valid_cli_retains_exclusive_lock_around_run(self):
        from contextlib import contextmanager
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); config = root / 'reader.json'
            config.write_text(json.dumps(dict(host='192.168.1.2', deviceID='enrolled')))
            locked = []
            @contextmanager
            def exclusive(path):
                self.assertEqual(path, root / 'build/dev-pipeline.lock')
                locked.append(True)
                try:
                    yield
                finally:
                    locked.pop()
            def run(args, config, job):
                self.assertEqual(locked, [True]); self.assertTrue(args.install_only)
                self.assertFalse(pipeline.install_only_argument_error(args))
            argv = ['dev_pipeline.py', 'run', '--install-only', '--flash', '--firmware', '/unused', '--config', str(config)]
            with patch.object(sys, 'argv', argv), patch.object(pipeline, 'ROOT', root), \
                 patch.object(pipeline, 'exclusive', side_effect=exclusive) as lock, \
                 patch.object(pipeline, 'run', side_effect=run) as invoke, \
                 patch.object(pipeline.os, 'umask'), patch.dict(pipeline.os.environ, {}, clear=True):
                pipeline.main()
            lock.assert_called_once(); invoke.assert_called_once(); self.assertEqual(locked, [])

    def test_identity_rechecked_immediately_before_install(self):
        with tempfile.TemporaryDirectory() as tmp:
            _, job, commands, calls, _, error = self.execute(Path(tmp), [self.status(), {**self.status(), 'deviceID': 'other'}])
            self.assertIsNotNone(error); self.assertEqual(commands, []); self.assertEqual(calls, 2)
            self.assertFalse(job.report['passed'])

    def test_install_error_stops_without_retry_or_success_probe(self):
        with tempfile.TemporaryDirectory() as tmp:
            _, job, commands, calls, _, error = self.execute(Path(tmp), [self.status(), self.status()], command_error=RuntimeError('lost flash outcome'))
            self.assertIsInstance(error, RuntimeError); self.assertEqual(len(commands), 1); self.assertEqual(calls, 2)
            self.assertFalse(job.report['passed']); self.assertEqual(job.report['stages'][-1]['state'], 'failed')

    def test_post_version_identity_or_mode_failure_never_passes(self):
        for post in [self.status('v-dev-wrong'), {**self.status('v-dev-next'), 'deviceID': 'other'},
                     {**self.status('v-dev-next'), 'mode': 'AP'}]:
            with self.subTest(post=post['version']), tempfile.TemporaryDirectory() as tmp:
                _, job, commands, calls, _, error = self.execute(Path(tmp), [self.status(), self.status(), post])
                self.assertIsNotNone(error); self.assertEqual(len(commands), 1); self.assertEqual(calls, 3)
                self.assertFalse(job.report['passed']); self.assertFalse(job.report['acceptancePerformed'])

    def test_running_companion_prevents_any_reader_or_install_io(self):
        with tempfile.TemporaryDirectory() as tmp:
            _, job, commands, calls, _, error = self.execute(Path(tmp), [], companion_error=RuntimeError('companion running'))
            self.assertIsNotNone(error); self.assertEqual(commands, []); self.assertEqual(calls, 0)
            self.assertFalse(job.report['passed'])

    def test_non_developer_artifact_is_rejected_before_install(self):
        with tempfile.TemporaryDirectory() as tmp:
            _, job, commands, calls, _, error = self.execute(Path(tmp), [self.status()], payload=b'CrossPoint version: 0.1.0\0')
            self.assertIsInstance(error, pipeline.pocket_put.ProtocolError)
            self.assertEqual(commands, []); self.assertEqual(calls, 1); self.assertFalse(job.report['passed'])

    def test_normal_run_without_new_flag_keeps_existing_behavior(self):
        self.assertIsNone(pipeline.install_only_argument_error(self.args(Path('/unused'), install_only=False, flash=False, firmware=None, cycles=2)))
        # Existing PipelineTest.test_pipeline_continues_hardware_suite_at_verified_new_address
        # executes the actual ordinary branch and requires its Mac hardware command/result.




class X4ContinuityTest(unittest.TestCase):
    def args(self, root, **changes):
        args = InstallOnlyTest().args(root, install_only=False, flash=False, x4_continuity=True)
        for name, value in changes.items(): setattr(args, name, value)
        return args

    def evidence(self):
        names = ['continuity-seed', 'continuity-app-to-x3-0', 'continuity-x3-to-app-0',
                 'continuity-app-to-x3-1', 'continuity-x3-to-app-1']
        return dict(passed=True, initialConnection=True, run='123', version='v-dev-current', platform='macOS',
            device='X4', model='X4', bluetoothCalls=0, identityVerified=True, sharedReaderExchangeIsolatedFromStartup=True,
            glanceSourcesUnconfigured=True, continuityValidation=dict(passed=True, restored=True, model='X4',
                transport='http', bluetoothCycles=[],
                captures=[dict(name=n, run=143, width=480, height=800, sha256='a'*64) for n in names],
                cases=[dict(direction='bidirectional', round=i, passed=True, transport='http',
                            backwardOffer=i==1, noMovementBeforeAcceptance=True,
                            acceptedCharacterVisible=True, navigatedFromAnotherChapter=True) for i in range(2)],
                epub=dict(kind='generated', sha256='b'*64, bytes=1000,
                          roundTripBytesMatch=True, libraryReimportMatches=True)))

    def test_scope_requires_explicit_artifact_and_rejects_every_other_action(self):
        root=Path('/unused')
        self.assertIsNone(pipeline.x4_continuity_argument_error(self.args(root)))
        for values in [dict(action='configure'),dict(firmware=None),dict(cycles=2),
                       dict(standby_seconds=91),dict(cppcheck='other')]:
            with self.subTest(values=values):self.assertIsNotNone(pipeline.x4_continuity_argument_error(self.args(root,**values)))
        for key in ('host','flash','install_only','build_firmware','checks','ios_simulator','ios_device',
                    'x4_recover_empty_revision','x4_recover_from','x4_sleep','x4_starters','x4_layout_only',
                    'x4_settings','x4_typography_only','x3_only','wifi_setup','real_epub','continuity_suite',
                    'ble_continuity','screen_suite','interface_suite','network_faults','standby','wake_control',
                    'delayed_advertising','battery_only'):
            with self.subTest(key=key):self.assertIsNotNone(pipeline.x4_continuity_argument_error(self.args(root,**{key:True})))
        self.assertIsNone(pipeline.x4_continuity_argument_error(self.args(root,x4_continuity=False,flash=True)))

    def test_invalid_cli_and_run_refuse_before_lock_or_io(self):
        with patch.object(sys,'argv',['dev_pipeline.py','run','--x4-continuity','--flash','--firmware','/unused']), \
             patch.object(pipeline,'exclusive',side_effect=AssertionError('no lock')):
            with self.assertRaises(SystemExit):pipeline.main()
        with patch.object(pipeline,'require_companion_stopped',side_effect=AssertionError('no process IO')):
            with self.assertRaises(ValueError):pipeline.run(self.args(Path('/unused'),flash=True),{},None)

    def test_exact_x4_http_generated_native_evidence_required(self):
        import copy
        good=self.evidence()
        pipeline.hardware_evidence(pipeline.MARKER+json.dumps(good),123,'v-dev-current',False,'macOS',continuity=True)
        pipeline.check_x4_continuity(good)
        for key,value in [('device','X3'),('model','X3'),('bluetoothCalls',1),('bluetoothCalls',False),('identityVerified',False),('sharedReaderExchangeIsolatedFromStartup',False),('glanceSourcesUnconfigured',False)]:
            bad=copy.deepcopy(good);bad[key]=value
            with self.subTest(key=key),self.assertRaises(RuntimeError):pipeline.check_x4_continuity(bad)
        for key,value in [('model','X3'),('transport','bluetooth'),('bluetoothCycles',[{}])]:
            bad=copy.deepcopy(good);bad['continuityValidation'][key]=value
            with self.subTest(key=key),self.assertRaises(RuntimeError):pipeline.check_x4_continuity(bad)
        for key,value in [('kind','published-book'),('sha256','z'*64),('sha256',None),('sha256',True),('sha256',7),('sha256',[]),('bytes',0),('bytes',True),('bytes',4*1024*1024+1),('roundTripBytesMatch',False),('libraryReimportMatches',False)]:
            bad=copy.deepcopy(good);bad['continuityValidation']['epub'][key]=value
            with self.subTest(key=key),self.assertRaises(RuntimeError):pipeline.check_x4_continuity(bad)
        for key,value in [('width',528),('height',792)]:
            bad=copy.deepcopy(good);bad['continuityValidation']['captures'][0][key]=value
            with self.subTest(key=key),self.assertRaises(RuntimeError):pipeline.check_x4_continuity(bad)
        bad=copy.deepcopy(good);bad['continuityValidation']['cases'][0]['transport']='bluetooth'
        with self.assertRaises(RuntimeError):pipeline.check_x4_continuity(bad)
        for key,value in [('name','foreign'),('run',144),('sha256','')]:
            bad=copy.deepcopy(good);bad['continuityValidation']['captures'][0][key]=value
            with self.assertRaises(RuntimeError):pipeline.hardware_evidence(pipeline.MARKER+json.dumps(bad),123,'v-dev-current',False,'macOS',continuity=True)

    def test_exported_bitmap_native_dimensions_not_just_report_claim(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);name='owned';data=bytearray(54);data[:2]=b'BM'
            data[18:22]=(480).to_bytes(4,'little',signed=True);data[22:26]=(800).to_bytes(4,'little',signed=True)
            (root/(name+'.bmp')).write_bytes(data)
            pipeline.check_x4_continuity_bitmaps(root,[dict(name=name)])
            for modified in [data[:25],b'XX'+data[2:],data[:18]+(528).to_bytes(4,'little')+data[22:],data[:22]+(792).to_bytes(4,'little')+data[26:]]:
                (root/(name+'.bmp')).write_bytes(modified)
                with self.assertRaises(RuntimeError):pipeline.check_x4_continuity_bitmaps(root,[dict(name=name)])

    def execute(self, root, *, model='X4', version='v-dev-current', hardware_fail=False, evidence_change=None):
        args=self.args(root);args.firmware.write_bytes(b'CrossPoint version: v-dev-current\0')
        job=pipeline.Pipeline(root/'run');commands=[];observed={}
        report=self.evidence()
        if evidence_change: report.update(evidence_change)
        statuses=[dict(deviceID='enrolled',device=model,mode='STA',version=version)]*3
        def command(name,argv,**kwargs):
            commands.append(name)
            def perform():
                log=job.output/(name+'.log');log.write_text('offline double\n')
                if name=='mac-build':
                    binary=root/'.build/dev-pipeline-mac/Build/Products/Debug/Pocket.app/Contents/MacOS/Pocket';binary.parent.mkdir(parents=True);binary.write_bytes(b'offline app')
                if name=='macOS-hardware-1':
                    observed.update(argv=argv,env=kwargs['env'],timeout=kwargs['timeout'])
                    if hardware_fail:
                        (job.output/'macOS-hardware-1.xcresult').mkdir()
                        raise RuntimeError('injected hardware failure')
                    log.write_text(pipeline.MARKER+json.dumps(report))
                return log
            return job.stage(name,perform)
        error=None
        with patch.object(pipeline,'source_state',return_value={'offline':True}), \
             patch.object(pipeline,'require_companion_stopped'), \
             patch.object(pipeline,'resolve_reader',side_effect=AssertionError('no selected-model discovery')), \
             patch.object(pipeline,'discover_with_app',side_effect=AssertionError('no discovery')), \
             patch.object(pipeline.cycle,'get_json',side_effect=statuses), \
             patch.object(pipeline.secrets,'randbelow',return_value=122) as random, \
             patch.object(pipeline,'collect_screen_artifacts') as export, \
             patch.object(pipeline,'check_x4_continuity_bitmaps') as bitmap, \
             patch.object(job,'command',side_effect=command):
            try:pipeline.run(args,dict(host='192.168.1.2',deviceID='enrolled'),job)
            except Exception as caught:error=caught
        return job,commands,observed,error,random,export,bitmap

    def test_only_x4_method_five_capture_export_and_reserved_run(self):
        with tempfile.TemporaryDirectory() as temp:
            job,commands,observed,error,random,export,bitmap=self.execute(Path(temp))
            self.assertIsNone(error);self.assertTrue(job.report['passed']);self.assertEqual(len(job.report['hardware']),1)
            self.assertIn('-only-testing:PocketMacTests/ReaderX4SettingsTests/testBidirectionalReading',observed['argv'])
            self.assertEqual(observed['env']['TEST_RUNNER_POCKET_HARDWARE_X4_CONTINUITY'],'1')
            self.assertEqual(observed['env']['TEST_RUNNER_POCKET_HARDWARE_MODEL'],'X4')
            for key in ('CONTINUITY','BLE_CONTINUITY','X4_SETTINGS','X4_STARTERS','X4_SLEEP'):
                self.assertEqual(observed['env']['TEST_RUNNER_POCKET_HARDWARE_'+key],'0')
            self.assertNotIn('upload-install-reboot',commands);self.assertIn('macOS-hardware-1-export-continuity',commands)
            self.assertEqual(observed['timeout'],1200);random.assert_called_once_with(0xffffffff-20)
            export.assert_called_once();bitmap.assert_called_once()

    def test_wrong_model_or_installed_version_never_runs_hardware(self):
        for changes in [dict(model='X3'),dict(version='v-dev-other')]:
            with tempfile.TemporaryDirectory() as temp:
                job,commands,_,error,_,_,_=self.execute(Path(temp),**changes)
                self.assertIsNotNone(error);self.assertFalse(job.report['passed']);self.assertNotIn('macOS-hardware-1',commands)
                self.assertNotIn('upload-install-reboot',commands)

    def test_pipeline_rejects_wrong_x4_result_before_export_or_pass(self):
        with tempfile.TemporaryDirectory() as temp:
            job,commands,_,error,_,export,bitmap=self.execute(Path(temp),evidence_change={'model':'X3'})
            self.assertIsInstance(error,RuntimeError);self.assertFalse(job.report['passed'])
            self.assertEqual(job.report['hardware'],[])
            self.assertNotIn('macOS-hardware-1-export-continuity',commands)
            export.assert_not_called();bitmap.assert_not_called()

    def test_failure_exports_evidence_and_never_claims_success(self):
        with tempfile.TemporaryDirectory() as temp:
            job,commands,_,error,_,export,bitmap=self.execute(Path(temp),hardware_fail=True)
            self.assertIsNotNone(error);self.assertFalse(job.report['passed'])
            self.assertEqual(commands.count('macOS-hardware-1'),1)
            self.assertIn('macOS-hardware-1-export-failed-continuity',commands)
            export.assert_not_called();bitmap.assert_not_called()

    def test_valid_x4_cli_keeps_exclusive_lock(self):
        from contextlib import contextmanager
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); config = root / 'reader.json'
            config.write_text(json.dumps(dict(host='192.168.1.2', deviceID='enrolled')))
            locked = []
            @contextmanager
            def exclusive(path):
                self.assertEqual(path, root / 'build/dev-pipeline.lock')
                locked.append(True)
                try:
                    yield
                finally:
                    locked.pop()
            def run(args, config, job):
                self.assertEqual(locked, [True]); self.assertTrue(args.x4_continuity)
                self.assertFalse(pipeline.x4_continuity_argument_error(args))
            argv = ['dev_pipeline.py', 'run', '--x4-continuity', '--firmware', '/unused', '--config', str(config)]
            with patch.object(sys, 'argv', argv), patch.object(pipeline, 'ROOT', root), \
                 patch.object(pipeline, 'exclusive', side_effect=exclusive) as lock, \
                 patch.object(pipeline, 'run', side_effect=run) as invoke, \
                 patch.object(pipeline.os, 'umask'), patch.dict(pipeline.os.environ, {}, clear=True):
                pipeline.main()
            lock.assert_called_once(); invoke.assert_called_once(); self.assertEqual(locked, [])


    def test_direct_script_entry_is_after_every_test_class(self):
        import ast
        tree=ast.parse(Path(__file__).read_text())
        guards=[(index,node) for index,node in enumerate(tree.body)
                if isinstance(node,ast.If) and isinstance(node.test,ast.Compare)
                and isinstance(node.test.left,ast.Name) and node.test.left.id=='__name__']
        self.assertEqual(len(guards),1)
        self.assertIs(guards[0][1],tree.body[-1])
        self.assertTrue(all(index<guards[0][0] for index,node in enumerate(tree.body) if isinstance(node,ast.ClassDef)))


if __name__ == '__main__':
    unittest.main()
