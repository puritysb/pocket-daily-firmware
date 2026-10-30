"""Deterministic offline tests; no sockets, apps, or readers are used."""
import argparse
import copy
import json
import io
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import zlib
import urllib.error

import dev_reader_walk as walk


def plan():
    data = b'PK unchanged synthetic EPUB bytes'
    return {'schema': 'pocket-dev-reader-walk-v1', 'host': '192.168.1.2', 'deviceID': 'TEST0001',
            'expectedVersion': '0.1.0-dev-test', 'model': 'X3', 'pages': 3,
            'fixtures': [{'label': 'en', 'source': '/synthetic.epub', 'local': 'en.epub', 'run': 123,
                          'target': '/Pocket visual 123.epub',
                          'staging': '/.pocket-11111111-2222-3333-4444-555555555555.part',
                          'bytes': len(data), 'sha256': walk.digest(data)}]}, data


def failure_record():
    data = bytearray(96)
    data[:7] = b'PDBF\x01\x01\x05'
    struct.pack_into('<HHIIII', data, 8, 7, 1, 99, 20000, 16000, 30)
    version = b'0.1.0-dev-test'
    data[28:28 + len(version)] = version
    struct.pack_into('<I', data, 92, zlib.crc32(data[:92]))
    return bytes(data)


class FakeClient:
    def __init__(self, p, data):
        self.plan, self.data = p, data
        self.fixture = p['fixtures'][0]
        self.status = {'deviceID': p['deviceID'], 'version': p['expectedVersion'], 'device': p['model'],
                       'mode': 'STA', 'firmwareLineage': 2, 'readerFiles': 2, 'readingProgress': 1,
                       'uploadStreamPort': 82, 'transferState': {'phase': 'idle'}}
        self.original = [{'name': 'original.epub', 'directory': False, 'size': 5, 'deletable': True}]
        self.book = False
        self.posted = 0
        self.uploaded = 0
        self.mutations = []
        self.lost_reply = False
        self.lost_commit = False
        self.bad_capture_run = False
        self.mutate_reading = False
        self.mismatch_after_post = False
        self.walk_error = None
        self.always_absent = False
        self.cleanup_reply_lost = False
        self.terminal_metadata = {}
        self.before_delete = lambda: None
        self.result = {'protocolVersion': 1, 'run': 123, 'state': 'completed', 'requestedPages': 3,
                       'renderedPages': 3, 'turns': 2, 'lastSpine': 1, 'lastPage': 1,
                       'elapsedMs': 3000, 'progressPercent': 3, 'failureStep': 0, 'failureDetail': 0,
                       'historyIsolated': True, 'progressMonotonic': True, 'captureReady': True}
        self.bmp = bytearray(62 + 66 * 792)
        self.bmp[:2] = b'BM'
        struct.pack_into('<I', self.bmp, 2, len(self.bmp))
        struct.pack_into('<ii', self.bmp, 18, 528, 792)

    def upload(self, status, data, path):
        self.uploaded += 1
        assert data == self.data
        assert path == self.fixture['staging']

    def get_json(self, path, query=None):
        if path == '/api/status':
            status = copy.deepcopy(self.status)
            if self.posted and self.mismatch_after_post:
                status['deviceID'] = 'OTHER001'
            return status
        if path == walk.API + 'dev/reader-capture':
            result = self.result if self.posted and not self.always_absent else {
                'protocolVersion': 1, 'run': 0, 'state': 'absent', 'historyIsolated': True}
            return {'activeRun': 0, 'pendingRun': 0, 'fixtureRun': 0, 'bookReady': self.book,
                    'completed': True, 'walk': copy.deepcopy(result),
                    'walkVersion': 1, 'maxWalkPages': 128,
                    'walkReceiptPresent': bool(self.posted), 'walkReceiptRun': 123 if self.posted else 0,
                    **(copy.deepcopy(self.terminal_metadata) if self.posted else {})}
        if path == walk.API + 'files':
            entries = self.original + ([{'name': self.fixture['target'][1:], 'directory': False,
                                        'size': len(self.data), 'deletable': True}] if self.book else [])
            return {'deviceID': self.plan['deviceID'], 'path': '/', 'entries': copy.deepcopy(entries), 'nextCursor': 0}
        if path == walk.API + 'preferences':
            return {'fontSize': 1, 'screenMargin': 5}
        if path == walk.API + 'reading':
            return {'deviceID': self.plan['deviceID'], 'books': [{'document': 'synthetic', 'seq':
                    2 if self.posted and self.mutate_reading else 1}]}
        if path in (walk.API + 'profile', walk.API + 'content/state'):
            return {'deviceID': self.plan['deviceID'], 'generation': 7}
        raise AssertionError(path)

    def request(self, method, path, query=None, body=None, expected=200, limit=walk.MAX_RESPONSE):
        query = query or {}
        if method != 'GET':
            self.mutations.append((method, path))
        if path == walk.API + 'dev/build-failure':
            if query.get('run') and self.walk_error:
                return self.walk_error, {'X-Build-Failure-Run': str(query['run'])}
            raise walk.HTTPFailure(404)
        if path == walk.API + 'commit':
            self.book = True
            if self.lost_commit:
                raise TimeoutError('synthetic lost commit reply')
            return json.dumps({'size': len(self.data), 'crc32': f'{zlib.crc32(self.data):08X}'}).encode(), {}
        if path == walk.API + 'files/content':
            assert query['path'] == self.fixture['target']
            assert self.book
            return self.data[query['offset']:query['offset'] + 4096], {}
        if path == walk.API + 'dev/reader-capture' and method == 'POST':
            assert query == {'run': 123, 'action': 'walk', 'pages': 3}
            self.posted += 1
            if self.lost_reply:
                raise TimeoutError('synthetic lost walk reply')
            return b'scheduled', {}
        if path == walk.API + 'dev/screen-capture' and method == 'GET':
            offset = query['offset']
            return bytes(self.bmp[offset:offset + 1024]), {
                'X-Capture-Run': '124' if self.bad_capture_run else '123', 'X-Capture-Size': str(len(self.bmp))}
        if method == 'DELETE':
            self.before_delete()
            if self.cleanup_reply_lost:
                raise TimeoutError('synthetic cleanup reply lost')
            if path == walk.API + 'files':
                self.book = False
                return json.dumps({'deviceID': self.plan['deviceID'], 'deleted': True}).encode(), {}
            return b'', {}
        raise AssertionError((method, path))


class WalkTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.plan, self.data = plan()
        walk.save_json(self.root / 'plan.json', self.plan)
        walk.save_bytes(self.root / 'en.epub', self.data)
        self.client = FakeClient(self.plan, self.data)
        self.t = 0
        self.walker = walk.Walker(self.root / 'plan.json', self.client, lambda: self.t, self.sleep)

    def sleep(self, seconds):
        self.t += seconds

    def execute(self):
        return self.walker.execute_fixture(self.plan['fixtures'][0])

    def assert_preserved_failure(self):
        with self.assertRaises(walk.WalkError):
            self.execute()
        self.assertFalse(any(method == 'DELETE' for method, _ in self.client.mutations))
        report = walk.load_json(self.root / 'en/report.json')
        self.assertFalse(report['passed'])
        self.assertTrue(report['remoteArtifactsPreserved'])

    def test_lost_walk_reply_observes_once_and_preserves_original(self):
        self.client.lost_reply = True
        report = self.execute()
        self.assertTrue(report['passed'])
        self.assertTrue(report['walkReplyLost'])
        self.assertEqual(self.client.posted, 1)
        self.assertEqual(self.client.uploaded, 1)
        self.assertEqual((self.root / 'en.epub').read_bytes(), self.data)
        self.assertFalse(self.client.book)
        self.assertEqual(len([x for x in self.client.mutations if x[0] == 'DELETE']), 3)
        self.assertEqual(walk.load_json(self.root / 'en/before.json'), walk.load_json(self.root / 'en/after-cleanup.json'))
        self.assertEqual((self.root / 'en/report.json').stat().st_mode & 0o777, 0o600)
        self.assertEqual((self.root / 'en').stat().st_mode & 0o777, 0o700)

    def test_lost_commit_uses_byte_readback_without_second_commit(self):
        self.client.lost_commit = True
        self.assertTrue(self.execute()['passed'])
        self.assertEqual(self.client.mutations.count(('POST', walk.API + 'commit')), 1)

    def image_metadata(self):
        return {'currentImageRendering': 2, 'imageDiagnosticsVersion': 1,
                'futureAdditiveField': {'preserved': True},
                'imageDiagnostics': {'version': 1, 'run': 123, 'complete': True, 'mode': 0, 'seen': 7,
                    'probeStreamFailures': 2, 'dimensionFailures': 1, 'lastProbeError': 5,
                    'extractAttempts': 4, 'extractFailures': 1, 'lastExtractError': 3,
                    'altFallbacks': 3, 'imageBlocks': 2,
                    'beforeProbeFreeHeap': 81234, 'beforeProbeLargestBlock': 45678}}

    def test_full_terminal_metadata_is_durable_before_cleanup_without_image_pass(self):
        self.client.terminal_metadata = self.image_metadata()
        saved = self.root / 'en/terminal-readiness.json'
        def before_delete():
            state = walk.load_json(saved)
            self.assertEqual(state, self.client.get_json(walk.API + 'dev/reader-capture', {'run': 123}))
            report = walk.load_json(self.root / 'en/report.json')
            self.assertEqual(report['terminalReadiness']['sha256'], walk.digest(saved.read_bytes()))
            self.assertEqual(report['terminalReadiness']['bytes'], len(saved.read_bytes()))
            self.assertEqual(report['imageValidation'], 'not-performed')
            self.assertTrue(report['imageVerificationPending'])
        self.client.before_delete = before_delete
        report = self.execute()
        self.assertTrue(report['passed'], 'Page-walk success remains separate from image validation')
        self.assertEqual(report['imageDiagnostics'], self.client.terminal_metadata['imageDiagnostics'])
        self.assertEqual(report['currentImageRendering'], 2, 'Current mode need not equal recorded walk mode')
        self.assertEqual(report['imageDiagnosticsValidation'], 'run-bound-metadata-only')
        self.assertEqual(saved.stat().st_mode & 0o777, 0o600)
        self.assertEqual(walk.load_json(saved)['futureAdditiveField'], {'preserved': True})

    def test_legacy_walk_without_image_diagnostics_remains_compatible(self):
        report = self.execute()
        self.assertTrue(report['passed'])
        self.assertEqual(report['imageValidation'], 'not-performed')
        self.assertEqual(report['imageDiagnosticsValidation'], 'not-reported')
        self.assertNotIn('imageDiagnostics', report)
        self.assertEqual(report['paintValidation'], 'not-performed')
        self.assertTrue(report['paintVerificationPending'])
        self.assertEqual(report['paintDiagnosticsValidation'], 'not-reported')
        self.assertNotIn('paintDiagnostics', report)
        self.assertTrue((self.root / 'en/terminal-readiness.json').is_file())

    def test_wrong_image_run_preserves_full_response_capture_and_remote_artifacts(self):
        self.client.terminal_metadata = self.image_metadata()
        self.client.terminal_metadata['imageDiagnostics']['run'] = 124
        self.client.walk_error = failure_record()
        self.assert_preserved_failure()
        saved = walk.load_json(self.root / 'en/terminal-readiness.json')
        self.assertEqual(saved['imageDiagnostics']['run'], 124)
        self.assertTrue((self.root / 'en/final-page.bmp').is_file())
        self.assertTrue((self.root / 'en/after-walk.json').is_file())
        self.assertEqual((self.root / 'en/walk-build-failure.bin').read_bytes(), failure_record())

    def test_advertised_missing_or_corrupt_diagnostics_are_not_legacy_absence(self):
        for value in (None, [], False):
            metadata = self.image_metadata()
            metadata['imageDiagnostics'] = value
            with self.subTest(value=value), self.assertRaises(walk.WalkError):
                walk.validate_image_diagnostics(metadata, self.plan['fixtures'][0])
        metadata.pop('imageDiagnostics')
        with self.assertRaises(walk.WalkError):
            walk.validate_image_diagnostics(metadata, self.plan['fixtures'][0])
        self.client.terminal_metadata = metadata
        self.assert_preserved_failure()
        self.assertTrue((self.root / 'en/terminal-readiness.json').is_file())
        self.assertTrue((self.root / 'en/final-page.bmp').is_file())

    def test_image_diagnostics_reject_wrong_types_versions_ranges_and_missing_fields(self):
        for key, value in [('version', True), ('version', 2), ('run', True), ('run', 124),
                           ('complete', False), ('complete', 1),
                           ('mode', -1), ('mode', 3),
                           ('seen', True), ('probeStreamFailures', -1), ('dimensionFailures', 0x100000000),
                           ('lastProbeError', 10), ('lastExtractError', '0'), ('beforeProbeFreeHeap', None)]:
            state = self.image_metadata()
            state['imageDiagnostics'][key] = value
            with self.subTest(key=key, value=value), self.assertRaises(walk.WalkError):
                walk.validate_image_diagnostics(state, self.plan['fixtures'][0])
        for key in self.image_metadata()['imageDiagnostics']:
            state = self.image_metadata()
            del state['imageDiagnostics'][key]
            with self.subTest(missing=key), self.assertRaises(walk.WalkError):
                walk.validate_image_diagnostics(state, self.plan['fixtures'][0])
        for key, value in [('currentImageRendering', True), ('currentImageRendering', 3),
                           ('imageDiagnosticsVersion', True), ('imageDiagnosticsVersion', 2)]:
            state = self.image_metadata()
            state[key] = value
            with self.subTest(key=key, value=value), self.assertRaises(walk.WalkError):
                walk.validate_image_diagnostics(state, self.plan['fixtures'][0])

    def paint_metadata(self):
        return {'currentReaderFontPoints': 17, 'paintDiagnosticsVersion': 1,
                'futurePaintField': {'preserved': True},
                'paintDiagnostics': {'version': 1, 'run': 123, 'complete': True,
                    'renderCalls': 8, 'cacheHits': 1, 'lazyExtractAttempts': 2, 'lazyExtractFailures': 1,
                    'lastLazyExtractError': 5, 'decoderAttempts': 5, 'decoderSuccesses': 3,
                    'placeholderCalls': 2, 'suppressedCalls': 1, 'pngAttempts': 4,
                    'pngEntryFreeHeap': 65432, 'pngEntryLargestBlock': 32100,
                    'pngObjectBytes': 23800, 'pngHeapGateBytes': 27900,
                    'firstFailureStage': 14, 'firstFailureCode': -6,
                    'firstFailureFreeHeap': 54321, 'firstFailureLargestBlock': 21000}}

    def test_paint_metadata_and_font_points_are_saved_before_cleanup_without_paint_pass(self):
        self.client.terminal_metadata = {**self.image_metadata(), **self.paint_metadata()}
        saved = self.root / 'en/terminal-readiness.json'
        def before_delete():
            state = walk.load_json(saved)
            self.assertEqual(state, self.client.get_json(walk.API + 'dev/reader-capture', {'run': 123}))
            report = walk.load_json(self.root / 'en/report.json')
            self.assertEqual(report['terminalReadiness']['sha256'], walk.digest(saved.read_bytes()))
            self.assertEqual(report['paintDiagnostics'], self.client.terminal_metadata['paintDiagnostics'])
            self.assertEqual(report['currentReaderFontPoints'], 17)
            self.assertEqual(report['paintValidation'], 'not-performed')
            self.assertTrue(report['paintVerificationPending'])
        self.client.before_delete = before_delete
        report = self.execute()
        self.assertTrue(report['passed'], 'Page-walk success does not prove successful image paint')
        self.assertEqual(report['paintDiagnosticsValidation'], 'run-bound-metadata-only')
        self.assertEqual(report['paintDiagnostics']['placeholderCalls'], 2)
        self.assertEqual(report['paintDiagnostics']['firstFailureCode'], -6)
        self.assertEqual(walk.load_json(saved)['futurePaintField'], {'preserved': True})
        self.assertEqual(saved.stat().st_mode & 0o777, 0o600)

    def test_wrong_paint_run_preserves_raw_response_pdbf_capture_and_state(self):
        self.client.terminal_metadata = self.paint_metadata()
        self.client.terminal_metadata['paintDiagnostics']['run'] = 124
        self.client.walk_error = failure_record()
        self.assert_preserved_failure()
        saved = walk.load_json(self.root / 'en/terminal-readiness.json')
        self.assertEqual(saved['paintDiagnostics']['run'], 124)
        self.assertEqual(walk.load_json(self.root / 'en/report.json')['error'], 'Paint diagnostic run mismatch')
        self.assertEqual((self.root / 'en/walk-build-failure.bin').read_bytes(), failure_record())
        self.assertTrue((self.root / 'en/final-page.bmp').is_file())
        self.assertTrue((self.root / 'en/after-walk.json').is_file())
        self.assertEqual(self.client.posted, 1)

    def test_supported_paint_record_missing_or_corrupt_is_not_legacy_absence(self):
        for value in (None, [], False, 'corrupt'):
            state = self.paint_metadata()
            state['paintDiagnostics'] = value
            with self.subTest(value=value), self.assertRaises(walk.WalkError):
                walk.validate_paint_diagnostics(state, self.plan['fixtures'][0])
        state.pop('paintDiagnostics')
        self.client.terminal_metadata = state
        self.assert_preserved_failure()
        self.assertNotIn('paintDiagnostics', walk.load_json(self.root / 'en/terminal-readiness.json'))
        self.assertTrue((self.root / 'en/final-page.bmp').is_file())
        self.assertTrue((self.root / 'en/after-walk.json').is_file())

    def test_incomplete_paint_record_stops_cleanup_after_evidence_is_saved(self):
        self.client.terminal_metadata = self.paint_metadata()
        self.client.terminal_metadata['paintDiagnostics']['complete'] = False
        self.assert_preserved_failure()
        self.assertIs(walk.load_json(self.root / 'en/terminal-readiness.json')['paintDiagnostics']['complete'], False)
        self.assertTrue((self.root / 'en/final-page.bmp').is_file())
        self.assertTrue((self.root / 'en/after-walk.json').is_file())

    def test_paint_fields_enforce_types_ranges_and_complete_schema(self):
        for key, value in [('version', True), ('version', 2), ('run', True), ('run', 124),
                           ('complete', 1), ('renderCalls', True), ('cacheHits', -1),
                           ('lazyExtractFailures', 0x100000000), ('lastLazyExtractError', 10),
                           ('decoderSuccesses', '3'), ('firstFailureStage', True), ('firstFailureStage', 17),
                           ('firstFailureCode', True), ('firstFailureCode', -(1 << 31) - 1),
                           ('firstFailureCode', 1 << 31), ('firstFailureFreeHeap', None)]:
            state = self.paint_metadata()
            state['paintDiagnostics'][key] = value
            with self.subTest(key=key, value=value), self.assertRaises(walk.WalkError):
                walk.validate_paint_diagnostics(state, self.plan['fixtures'][0])
        for key in self.paint_metadata()['paintDiagnostics']:
            state = self.paint_metadata()
            del state['paintDiagnostics'][key]
            with self.subTest(missing=key), self.assertRaises(walk.WalkError):
                walk.validate_paint_diagnostics(state, self.plan['fixtures'][0])
        for capability in (True, 0, 2, '1', None):
            state = self.paint_metadata()
            state['paintDiagnosticsVersion'] = capability
            with self.subTest(capability=capability), self.assertRaises(walk.WalkError):
                walk.validate_paint_diagnostics(state, self.plan['fixtures'][0])
        for code in (-(1 << 31), 0, (1 << 31) - 1):
            state = self.paint_metadata()
            state['paintDiagnostics']['firstFailureCode'] = code
            self.assertEqual(walk.validate_paint_diagnostics(state, self.plan['fixtures'][0]), 'run-bound-metadata-only')

    def test_paint_absent_observation_fields_are_consistent_without_inventing_success(self):
        state = self.paint_metadata()
        record = state['paintDiagnostics']
        for selector, fields in [('firstFailureStage', ('firstFailureCode', 'firstFailureFreeHeap', 'firstFailureLargestBlock')),
                                 ('pngAttempts', ('pngEntryFreeHeap', 'pngEntryLargestBlock', 'pngObjectBytes', 'pngHeapGateBytes'))]:
            original = dict(record)
            record[selector] = 0
            for field in fields:
                record[field] = 0
            self.assertEqual(walk.validate_paint_diagnostics(state, self.plan['fixtures'][0]), 'run-bound-metadata-only')
            for field in fields:
                record[field] = 1
                with self.subTest(field=field), self.assertRaises(walk.WalkError):
                    walk.validate_paint_diagnostics(state, self.plan['fixtures'][0])
                record[field] = 0
            record.update(original)
        self.assertEqual(walk.validate_paint_diagnostics({'currentReaderFontPoints': 15}, self.plan['fixtures'][0]),
                         'not-reported', 'Additive current font setting alone is not paint support')

    def test_paint_counter_relationships_match_record_integrity_not_page_or_pixel_counts(self):
        for key, value in [('cacheHits', 9), ('lazyExtractAttempts', 9), ('lazyExtractFailures', 3),
                           ('decoderAttempts', 9), ('decoderSuccesses', 6), ('placeholderCalls', 9),
                           ('suppressedCalls', 3), ('pngAttempts', 6)]:
            state = self.paint_metadata()
            state['paintDiagnostics'][key] = value
            with self.subTest(key=key), self.assertRaisesRegex(walk.WalkError, 'Inconsistent paint diagnostic counters'):
                walk.validate_paint_diagnostics(state, self.plan['fixtures'][0])
        state = self.paint_metadata()
        state['paintDiagnostics']['renderCalls'] = 0xffffffff
        self.assertEqual(walk.validate_paint_diagnostics(state, self.plan['fixtures'][0]), 'run-bound-metadata-only',
                         'Multiple paint passes need not match requested pages or layout image blocks')

    def test_terminal_write_failure_cannot_reach_cleanup(self):
        original_save = walk.save_json
        def fail_terminal(path, value):
            if Path(path).name == 'terminal-readiness.json':
                raise OSError('synthetic disk failure')
            original_save(path, value)
        with patch.object(walk, 'save_json', side_effect=fail_terminal), self.assertRaises(OSError):
            self.execute()
        self.assertFalse(any(method == 'DELETE' for method, _ in self.client.mutations))
        self.assertFalse(walk.load_json(self.root / 'en/report.json')['passed'])

    def test_production_refused_before_upload_or_post(self):
        self.client.status['version'] = '0.1.0'
        self.assert_preserved_failure()
        self.assertEqual(self.client.uploaded, 0)
        self.assertEqual(self.client.mutations, [])

    def test_identity_model_mode_and_lineage_guards(self):
        for key, value in [('deviceID', 'OTHER001'), ('device', 'X4'), ('mode', 'AP'), ('firmwareLineage', 1)]:
            with self.subTest(key=key):
                status = dict(self.client.status, **{key: value})
                with self.assertRaises(walk.WalkError):
                    walk.verify_status(status, self.plan)

    def test_legacy_completed_without_walk_is_not_success(self):
        with patch.object(self.client, 'get_json', return_value={'completed': True}):
            with self.assertRaises(walk.WalkError):
                walk.readiness(self.client, 123)

    def test_existing_fixture_never_overwritten(self):
        self.client.book = True
        self.assert_preserved_failure()
        self.assertEqual(self.client.uploaded, 0)

    def test_unrelated_or_invalid_walk_receipt_blocks_preflight(self):
        ready = self.client.get_json(walk.API + 'dev/reader-capture', {'run': 123})
        for run in (0, 124):
            with self.assertRaises(walk.WalkError):
                walk.fresh_readiness(dict(ready, walkReceiptPresent=True, walkReceiptRun=run), 123)
        without_flag = dict(ready)
        without_flag.pop('walkReceiptPresent')
        with self.assertRaises(walk.WalkError):
            walk.fresh_readiness(without_flag, 123)

    def test_other_run_never_certifies_current(self):
        self.client.result['run'] = 124
        self.assert_preserved_failure()

    def test_timeout_keeps_fixture_and_diagnostics(self):
        self.client.result['state'] = 'timed-out'
        self.assert_preserved_failure()
        self.assertTrue(self.client.book)
        self.assertTrue((self.root / 'en/final-page.bmp').is_file())
        report = walk.load_json(self.root / 'en/report.json')
        self.assertTrue(report['publicStateUnchangedBeforeCleanup'])

    def test_missing_terminal_receipt_is_bounded_without_post_retry(self):
        self.client.always_absent = True
        self.assert_preserved_failure()
        self.assertEqual(self.client.posted, 1)
        self.assertGreaterEqual(self.t, 720)

    def test_short_or_nonmonotonic_walk_is_not_pass(self):
        for change in ({'renderedPages': 2}, {'turns': 1}, {'progressMonotonic': False}, {'historyIsolated': False},
                       {'captureReady': False}, {'failureStep': 1}, {'failureDetail': 5},
                       {'turns': True}, {'progressPercent': float('nan')}, {'progressPercent': True}, {'lastPage': 0}):
            with self.subTest(change=change), self.assertRaises(walk.WalkError):
                walk.validate_result(dict(self.client.result, **change), self.plan['fixtures'][0], 3)

    def test_wrong_reader_after_reboot_stops_before_capture_or_cleanup(self):
        self.client.mismatch_after_post = True
        self.assert_preserved_failure()

    def test_stale_capture_cannot_certify_walk(self):
        self.client.bad_capture_run = True
        self.assert_preserved_failure()

    def test_new_diagnostic_is_failure_even_if_completed_claimed(self):
        self.client.walk_error = failure_record()
        self.assert_preserved_failure()
        self.assertEqual((self.root / 'en/walk-build-failure.bin').read_bytes(), failure_record())

    def test_original_history_change_never_gets_cleaned_or_passed(self):
        self.client.mutate_reading = True
        self.assert_preserved_failure()

    def test_repeat_run_refused_without_repeating_post(self):
        self.execute()
        with self.assertRaises(walk.WalkError):
            self.execute()
        self.assertEqual(self.client.posted, 1)

    def test_cleanup_unknown_outcome_preserves_evidence_without_retry(self):
        self.client.cleanup_reply_lost = True
        with self.assertRaises(TimeoutError):
            self.execute()
        report = walk.load_json(self.root / 'en/report.json')
        self.assertFalse(report['passed'])
        self.assertTrue(report['cleanupMayBePartial'])
        self.assertFalse(report['remoteArtifactsPreserved'])
        self.assertTrue(report['localRecoveryEvidencePreserved'])
        self.assertTrue((self.root / 'en/final-page.bmp').is_file())
        self.assertEqual(len([x for x in self.client.mutations if x[0] == 'DELETE']), 1)

    def test_frozen_source_tampering_refused(self):
        (self.root / 'en.epub').write_bytes(b'changed')
        self.assert_preserved_failure()
        self.assertEqual(self.client.mutations, [])

    def test_diagnostic_crc_and_size_are_checked(self):
        self.assertEqual(walk.parse_build_failure(failure_record())['stepName'], 'html-stream')
        for bad in (failure_record()[:-1], failure_record() + b'x', b'X' + failure_record()[1:],
                    failure_record()[:50] + b'x' + failure_record()[51:]):
            with self.assertRaises(walk.WalkError):
                walk.parse_build_failure(bad)

    def test_diagnostic_wrong_run_header_is_rejected(self):
        with patch.object(self.client, 'request', return_value=(failure_record(), {'X-Build-Failure-Run': '124'})):
            with self.assertRaises(walk.WalkError):
                walk.diagnostic(self.client, 123)

    def test_plan_is_offline_and_preserves_source_bytes(self):
        source = self.root / 'source.epub'
        source.write_bytes(self.data)
        config = self.root / 'config.json'
        walk.save_json(config, {'host': self.plan['host'], 'deviceID': self.plan['deviceID']})
        args = argparse.Namespace(config=config, output=self.root / 'offline', expected_version=self.plan['expectedVersion'],
                                  model='X3', pages=20, fixture=[f'en={source}'])
        with patch.object(walk, 'Client', side_effect=AssertionError('offline plan must not create transport')):
            result = walk.create_plan(args)
        self.assertFalse(walk.load_json(result)['deviceIOPerformed'])
        self.assertEqual((result.parent / 'en.epub').read_bytes(), source.read_bytes())
        self.assertEqual(result.stat().st_mode & 0o777, 0o600)

    def test_opt_in_required_before_constructing_transport(self):
        with patch.object(walk, 'Walker', side_effect=AssertionError('no opt-in transport')):
            self.assertEqual(walk.main(['observe', '--plan', str(self.root / 'plan.json'), '--label', 'en']), 2)

    def test_plan_rejects_traversal_and_page_bounds(self):
        for change in ({'pages': 0}, {'pages': 129}, {'expectedVersion': '0.1.0'}):
            with self.assertRaises(walk.WalkError):
                walk.valid_plan(dict(self.plan, **change))
        changed = copy.deepcopy(self.plan)
        changed['fixtures'][0]['local'] = '../outside.epub'
        with self.assertRaises(walk.WalkError):
            walk.valid_plan(changed)


class DiagnosticHTTPTests(unittest.TestCase):
    class Response(io.BytesIO):
        def __init__(self, status=200, data=b'ok'):
            super().__init__(data)
            self.status, self.headers = status, {'Synthetic': 'yes'}
            self.read_sizes = []

        def read(self, size=-1):
            self.read_sizes.append(size)
            return super().read(size)

    def client(self, outcomes, *, pause_jump=1, advance=0):
        self.t, self.calls, self.pauses = 0, [], []
        owner = self

        class Opener:
            def open(self, request, timeout):
                owner.calls.append((request, timeout))
                owner.t += advance
                owner.assertTrue(outcomes, 'Unexpected request replay')
                value = outcomes.pop(0)
                if isinstance(value, BaseException):
                    raise value
                return value

        def pause(seconds):
            owner.pauses.append(seconds)
            owner.t += pause_jump

        p, _ = plan()
        return walk.Client(p, opener=Opener(), clock=lambda: self.t, sleep=pause)

    def error(self, code=409, body=b'Reader is drawing'):
        stream = self.Response(data=body)
        failure = urllib.error.HTTPError('http://private.invalid/never-log?deviceID=PRIVATE1', code,
                                         'private exception reason', {}, stream)
        return failure, stream

    def test_exact_drawing_gets_retry_same_bound_request_then_return(self):
        for path, body in walk.DRAWING_GET_MESSAGES.items():
            first, first_stream = self.error(body=body)
            second, second_stream = self.error(body=body)
            response = self.Response(data=b'original bytes')
            client = self.client([first, second, response], advance=0.25)
            content, headers = client.request('GET', path, {'run': 123, 'offset': 4096, 'path': '/owned.epub'})
            self.assertEqual(content, b'original bytes')
            self.assertEqual(headers, {'Synthetic': 'yes'})
            self.assertEqual(len(self.calls), 3)
            self.assertEqual(self.pauses, [1, 1])
            self.assertTrue(all(request is self.calls[0][0] for request, _ in self.calls))
            self.assertIn('deviceID=TEST0001', self.calls[0][0].full_url)
            self.assertIn('offset=4096', self.calls[0][0].full_url)
            self.assertIsNone(self.calls[0][0].data)
            self.assertTrue(all(0 < timeout <= 10 for _, timeout in self.calls))
            self.assertTrue(first_stream.closed and second_stream.closed and response.closed)

    def test_retry_requires_exact_route_method_code_body_and_expected_status(self):
        reader = walk.API + 'dev/reader-capture'
        content = walk.API + 'files/content'
        cases = [('GET', reader, 200, 409, b'Another file transfer is active'),
                 ('GET', reader, 200, 409, b'Reader is drawing\n'),
                 ('GET', reader, 200, 409, b'Reader is drawing; retry the download'),
                 ('GET', content, 200, 409, b'Reader is drawing'),
                 ('GET', reader, 200, 503, b'Reader is drawing'),
                 ('GET', walk.API + 'dev/build-failure', 200, 409, b'Reader is drawing\n'),
                 ('POST', walk.API + 'dev/build-failure', 200, 409, b'Reader is drawing'),
                 ('GET', walk.API + 'dev/screen-capture', 200, 409, b'Reader is drawing'),
                 ('GET', '/api/status', 200, 409, b'Reader is drawing'),
                 ('POST', reader, 202, 409, b'Reader is drawing'),
                 ('POST', reader, 200, 409, b'Reader is drawing'),
                 ('POST', walk.API + 'commit', 200, 409, b'Reader is drawing'),
                 ('DELETE', reader, 204, 409, b'Reader is drawing'),
                 ('DELETE', reader, 200, 409, b'Reader is drawing'),
                 ('GET', reader, 409, 409, b'Reader is drawing')]
        for method, path, expected, code, body in cases:
            with self.subTest(method=method, path=path, code=code, body=body, expected=expected):
                error, stream = self.error(code, body)
                client = self.client([error])
                with self.assertRaises(walk.HTTPFailure) as caught:
                    client.request(method, path, {'run': 123}, expected=expected)
                self.assertEqual(caught.exception.code, code)
                self.assertEqual(len(self.calls), 1)
                self.assertEqual(self.pauses, [])
                self.assertTrue(stream.closed)

    def test_build_failure_get_survives_drawing_without_replaying_a_mutation(self):
        busy, stream = self.error()
        response = self.Response(data=b'run-bound diagnostic')
        client = self.client([busy, response])
        body, _ = client.request('GET', walk.API + 'dev/build-failure', {'run': 321}, limit=96)
        self.assertEqual(body, b'run-bound diagnostic')
        self.assertEqual(len(self.calls), 2)
        self.assertEqual(self.pauses, [1])
        self.assertTrue(stream.closed and response.closed)
        self.assertTrue(all(request.get_method() == 'GET' and request is self.calls[0][0]
                            and 'run=321' in request.full_url for request, _ in self.calls))

    def test_attempt_bound_is_six_and_does_not_read_seventh_response(self):
        errors = [self.error()[0] for _ in range(6)]
        client = self.client(errors)
        with self.assertRaises(walk.HTTPFailure) as caught:
            client.request('GET', walk.API + 'dev/reader-capture')
        self.assertEqual(caught.exception.attempts, 6)
        self.assertEqual(len(self.calls), 6)
        self.assertEqual(self.pauses, [1] * 5)

    def test_deadline_counts_requests_and_backoff(self):
        for advance, pause_jump, expected_pauses in [(0, 10, 1), (9, 1, 0)]:
            client = self.client([self.error()[0]], advance=advance, pause_jump=pause_jump)
            with self.assertRaises(walk.HTTPFailure) as caught:
                client.request('GET', walk.API + 'dev/reader-capture')
            self.assertEqual(caught.exception.attempts, 1)
            self.assertEqual(len(self.calls), 1)
            self.assertEqual(len(self.pauses), expected_pauses)

    def test_success_after_read_deadline_does_not_claim_in_budget_completion(self):
        response = self.Response()
        client = self.client([response], advance=10)
        with self.assertRaisesRegex(walk.WalkError, 'read budget expired'):
            client.request('GET', walk.API + 'dev/reader-capture')
        self.assertEqual(len(self.calls), 1)
        self.assertTrue(response.closed)

    def test_unknown_error_body_is_bounded_redacted_and_has_no_private_url(self):
        body = b'PRIVATE1 secret-user-book.epub http://private.invalid token ' * 1000
        error, stream = self.error(body=body)
        client = self.client([error])
        with self.assertRaises(walk.HTTPFailure) as caught:
            client.request('POST', walk.API + 'commit', {'deviceID': 'TEST0001', 'path': '/private-book.epub'},
                           body={'staging': 'private staging', 'target': 'private target'})
        failure = caught.exception
        self.assertEqual(stream.read_sizes, [walk.MAX_ERROR_BODY + 1])
        self.assertEqual(failure.body_bytes_read, 257)
        self.assertTrue(failure.body_truncated)
        self.assertFalse(failure.body_complete)
        self.assertEqual(failure.body, '[redacted]')
        self.assertIn('POST /api/pocket/v1/commit: HTTP 409', str(failure))
        for private in ('PRIVATE1', 'TEST0001', 'private', 'http://', '?', 'staging', 'target'):
            self.assertNotIn(private, str(failure))
        self.assertTrue(stream.closed)

    def test_unsafe_path_or_method_cannot_enter_error_diagnostic(self):
        failure = walk.HTTPFailure(409, method='GET PRIVATE1', path='http://secret/deviceID=PRIVATE1',
                                   body=b'PRIVATE1')
        self.assertEqual(failure.method, 'unknown-method')
        self.assertEqual(failure.path, 'unknown-endpoint')
        self.assertNotIn('PRIVATE1', str(failure))
        self.assertNotIn('secret', str(failure))

    def test_error_body_read_failure_is_not_retried(self):
        error, stream = self.error()
        client = self.client([error])
        with patch.object(stream, 'read', side_effect=OSError('private transport reason')):
            with self.assertRaises(walk.HTTPFailure) as caught:
                client.request('GET', walk.API + 'dev/reader-capture')
        self.assertFalse(caught.exception.body_complete)
        self.assertEqual(len(self.calls), 1)
        self.assertEqual(self.pauses, [])
        self.assertTrue(stream.closed)

    def test_transport_errors_and_cancelled_backoff_never_replay(self):
        for error in (TimeoutError('synthetic'), urllib.error.URLError('synthetic'), KeyboardInterrupt()):
            client = self.client([error])
            with self.assertRaises(type(error)):
                client.request('GET', walk.API + 'dev/reader-capture')
            self.assertEqual(len(self.calls), 1)
            self.assertEqual(self.pauses, [])
        client = self.client([self.error()[0]])
        with patch.object(client, 'sleep', side_effect=KeyboardInterrupt()):
            with self.assertRaises(KeyboardInterrupt):
                client.request('GET', walk.API + 'dev/reader-capture')
        self.assertEqual(len(self.calls), 1)

    def test_get_body_disables_retry_and_unknown_success_status_is_diagnostic(self):
        client = self.client([self.error()[0]])
        with self.assertRaises(walk.HTTPFailure):
            client.request('GET', walk.API + 'dev/reader-capture', body={})
        self.assertEqual(len(self.calls), 1)
        response = self.Response(status=201, data=b'private contents')
        client = self.client([response])
        with self.assertRaises(walk.HTTPFailure) as caught:
            client.request('POST', walk.API + 'commit')
        self.assertEqual(caught.exception.code, 201)
        self.assertEqual(caught.exception.body, '[redacted]')
        self.assertTrue(response.closed)

    def test_json_and_bounded_success_read_keep_existing_contract(self):
        client = self.client([self.error()[0], self.Response(data=b'{"run":123}')])
        self.assertEqual(client.get_json(walk.API + 'dev/reader-capture'), {'run': 123})
        response = self.Response(data=b'12345')
        client = self.client([response])
        with self.assertRaisesRegex(walk.WalkError, 'Response exceeds bounded size'):
            client.request('GET', walk.API + 'files/content', limit=4)
        self.assertEqual(response.read_sizes, [5])
        self.assertTrue(response.closed)

    def test_redirect_remains_refused_and_original_method_is_retained(self):
        owner = self
        class RedirectOpener:
            def open(self, request, timeout):
                owner.calls.append((request, timeout))
                return walk.NoRedirect().redirect_request(request, io.BytesIO(b'private redirect body'), 302,
                                                          'redirect', {}, 'http://private-target.invalid')
        client = self.client([])
        client.opener = RedirectOpener()
        with self.assertRaises(walk.HTTPFailure) as caught:
            client.request('POST', walk.API + 'commit', body={'private': 'value'})
        self.assertEqual(caught.exception.method, 'POST')
        self.assertEqual(caught.exception.path, walk.API + 'commit')
        self.assertEqual(caught.exception.code, 302)
        self.assertNotIn('private', str(caught.exception))
        self.assertEqual(len(self.calls), 1)

    def test_failure_report_retains_context_without_starting_walk_or_cleanup(self):
        p, data = plan()
        client = FakeClient(p, data)
        original_request = client.request
        def fail_commit(method, path, *args, **kwargs):
            if method == 'POST' and path == walk.API + 'commit':
                raise walk.HTTPFailure(409, method=method, path=path, body=b'Reader identity mismatch')
            return original_request(method, path, *args, **kwargs)
        client.request = fail_commit
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            walk.save_json(root / 'plan.json', p)
            walk.save_bytes(root / 'en.epub', data)
            walker = walk.Walker(root / 'plan.json', client, lambda: 0, lambda _: None)
            with self.assertRaises(walk.HTTPFailure):
                walker.execute_fixture(p['fixtures'][0])
            report = walk.load_json(root / 'en/report.json')
            self.assertEqual(report['stageAtFailure'], 'commit-submission-prepared')
            self.assertFalse(report['submitted'])
            self.assertFalse(report['passed'])
            self.assertIn('POST /api/pocket/v1/commit: HTTP 409', report['error'])
            self.assertIn('Reader identity mismatch', report['error'])
            self.assertNotIn(p['deviceID'], report['error'])
            self.assertEqual(client.posted, 0)
            self.assertFalse(any(method == 'DELETE' for method, _ in client.mutations))
            self.assertEqual((root / 'en/report.json').stat().st_mode & 0o777, 0o600)


if __name__ == '__main__':
    unittest.main()
