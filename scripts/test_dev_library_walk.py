"""Offline only: synthetic EPUB, private temporary files and injected transport."""
import copy
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import dev_reader_walk as walk
import dev_library_walk as lib
from test_dev_reader_walk import FakeClient, plan as base_plan, failure_record


def fixture():
    output = io.BytesIO()
    with zipfile.ZipFile(output, 'w') as z:
        z.writestr('mimetype', 'application/epub+zip')
        z.writestr('META-INF/container.xml', '<container><rootfiles><rootfile full-path="book.opf"/></rootfiles></container>')
        z.writestr('book.opf', '<package xmlns:dc="http://purl.org/dc/elements/1.1/"><metadata><dc:title>Pocket RC 한국어 읽기</dc:title></metadata></package>')
    data = output.getvalue()
    p, _ = base_plan()
    p.update(libraryTrialVersion=1, referencePath='/original-ko.epub', titleSHA256=walk.digest('Pocket RC 한국어 읽기'.encode()))
    p['fixtures'][0].update(bytes=len(data), sha256=walk.digest(data))
    return p, data


class LibraryClient(FakeClient):
    def __init__(self, p, data):
        super().__init__(p, data)
        self.original[0].update(name='original-ko.epub', size=len(data))
        self.cleaned = False
        self.acked = False
        self.library_error = False
        self.bad_raw = False
        self.bad_after_cleanup = False
        self.ack_reply_lost = False
        self.bad_backup_header = False
        self.bad_raw_hash = False
        self.bad_original = False
        self.missing_frame = False
        self.raw = {'state': b'original-state', 'recent': b'{"books":[]}', 'settings': b'original-settings',
                    'index': b'original-derived-index', 'dirty': None}
        self.post_body = None
        self.get_frames = []

    def manifest(self):
        rows = []
        for name in lib.SELECTORS:
            data = self.raw[name]
            if name == 'index' and self.cleaned:
                data = b'rebuilt-original-derived-index'
            if name == 'state' and self.posted and (self.bad_raw or self.cleaned and self.bad_after_cleanup):
                data = b'CHANGED-original-state'
            rows.append({'selector': name, 'present': data is not None, 'bytes': len(data) if data is not None else 0,
                         'sha256': walk.digest(data) if data is not None else '0' * 64})
        joined = b''.join(struct.pack('<BBI', i, row['present'], row['bytes']) + bytes.fromhex(row['sha256'])
                          for i, row in enumerate(rows[:4]))
        return {'version': 1, 'run': 123, 'files': rows, 'backupDigest': walk.digest(joined)}

    def get_json(self, path, query=None):
        query = query or {}
        if path == lib.API and query.get('libraryBackup') == 'manifest':
            return self.manifest()
        value = super().get_json(path, query)
        if path == lib.API:
            value['libraryTrialVersion'] = 1
            value['library'] = {'version': 1, 'present': False, 'valid': False}
            if self.posted and not self.acked:
                value['library'] = {'version': 1, 'run': 123, 'present': True, 'valid': True,
                    'phase': 9 if self.cleaned else 7 if self.library_error else 6,
                    'error': 4 if self.library_error else 0, 'flags': 15, 'openCount': 1,
                    'originalRow': 2, 'ownedRow': 0, 'elapsedMs': 3000,
                    'originalCapture': not self.missing_frame, 'ownedCapture': True, 'derivedIndexMayChange': True}
            if self.acked:
                value.update(walkReceiptPresent=False, walkReceiptRun=0)
                value['walk'] = {'protocolVersion': 1, 'run': 0, 'state': 'absent', 'historyIsolated': True}
        return value

    def request(self, method, path, query=None, body=None, **kwargs):
        query = query or {}
        if path == lib.API and query.get('libraryBackup'):
            name = query['libraryBackup']; offset = query['offset']
            data = self.raw[name]
            if name == 'index' and self.cleaned:
                data = b'rebuilt-original-derived-index'
            if name == 'state' and self.posted and (self.bad_raw or self.cleaned and self.bad_after_cleanup):
                data = b'CHANGED-original-state'
            if self.bad_raw_hash:
                data = b'x' * len(data)
            return data[offset:offset + 1024], {'X-Backup-Run': '124' if self.bad_backup_header else '123',
                'X-Backup-Total': str(len(data)), 'X-Backup-Offset': str(offset)}
        if path == walk.API + 'files/content' and query['path'] == self.plan['referencePath']:
            data = b'x' * len(self.data) if self.bad_original else self.data
            return data[query['offset']:query['offset'] + 4096], {}
        if path == lib.API and method == 'POST':
            self.mutations.append((method, path))
            assert query == {'run': 123, 'action': 'library-walk'}
            self.posted += 1
            self.post_body = body
            if self.lost_reply:
                raise TimeoutError('synthetic POST reply loss')
            return b'', {}
        if path == lib.API and method == 'DELETE':
            self.mutations.append((method, path))
            self.before_delete()
            if query.get('libraryAck') == 'true':
                self.acked = True
                if self.ack_reply_lost:
                    raise TimeoutError('synthetic ACK reply loss')
            else:
                self.cleaned = True
                self.book = False
                if self.cleanup_reply_lost:
                    raise TimeoutError('synthetic DELETE reply loss')
            return b'', {}
        if path == walk.API + 'dev/screen-capture':
            self.get_frames.append(query.get('frame', 'reader'))
        return super().request(method, path, query, body, **kwargs)


class LibraryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.plan, self.data = fixture()
        walk.save_json(self.root / 'plan.json', self.plan)
        walk.save_bytes(self.root / 'en.epub', self.data)
        self.client = LibraryClient(self.plan, self.data)
        self.t = 0
        self.runner = lib.LibraryWalker(self.root / 'plan.json', client=self.client, clock=lambda: self.t, sleep=self.sleep)

    def sleep(self, seconds):
        self.t += seconds

    def run_trial(self):
        return self.runner.execute_fixture(self.plan['fixtures'][0])

    def no_delete(self, expected):
        with self.assertRaisesRegex(walk.WalkError, expected):
            self.run_trial()
        self.assertFalse(any(m == 'DELETE' for m, _ in self.client.mutations))
        report = walk.load_json(self.root / 'en/report.json')
        self.assertFalse(report['passed'])
        return report

    def test_success_saves_all_frames_and_raw_bytes_before_single_cleanup_ack(self):
        def durable():
            folder = self.root / 'en'
            for name in ('library-original.bmp', 'library-owned.bmp', 'final-page.bmp', 'terminal-readiness.json',
                         'raw-before/settings.bin', 'raw-after-trial/manifest.json'):
                self.assertTrue((folder / name).exists(), name)
                self.assertEqual((folder / name).stat().st_mode & 0o777, 0o600)
        self.client.before_delete = durable
        report = self.run_trial()
        self.assertTrue(report['passed'])
        self.assertEqual(self.client.posted, 1)
        self.assertEqual(self.client.uploaded, 1)
        self.assertEqual(sum(m == 'DELETE' for m, _ in self.client.mutations), 2)
        self.assertEqual(set(self.client.get_frames), {'library-original', 'library-owned', 'reader'})
        self.assertEqual(self.client.post_body['titleSHA256'], self.plan['titleSHA256'])
        self.assertFalse(report['productionAcceptance'])
        self.assertFalse(report['opticalAcceptance'])
        self.assertEqual(report['libraryFrameValidation'], 'not-performed')
        self.assertFalse(report['derivedIndexByteIdentical'])
        self.assertTrue(report['rawProtectedStateUnchanged'])
        self.assertEqual((self.root / 'en.epub').read_bytes(), self.data)

    def test_lost_post_observes_without_replay(self):
        self.client.lost_reply = True
        self.assertTrue(self.run_trial()['passed'])
        self.assertEqual(self.client.posted, 1)

    def test_lost_cleanup_reply_readback_does_not_repeat_delete(self):
        self.client.cleanup_reply_lost = True
        result = self.run_trial()
        self.assertTrue(result['passed'])
        self.assertTrue(result['cleanupReplyLost'])
        self.assertEqual(sum(m == 'DELETE' for m, _ in self.client.mutations), 2)

    def test_original_state_changed_after_cleanup_prevents_ack(self):
        self.client.bad_after_cleanup = True
        with self.assertRaisesRegex(walk.WalkError, 'Original state changed after cleanup'):
            self.run_trial()
        self.assertTrue(self.client.cleaned)
        self.assertFalse(self.client.acked)
        self.assertEqual(sum(m == 'DELETE' for m, _ in self.client.mutations), 1)
        self.assertTrue((self.root / 'en/raw-after-cleanup/state.bin').exists())

    def test_ack_reply_lost_is_never_replayed(self):
        self.client.ack_reply_lost = True
        with self.assertRaises(TimeoutError):
            self.run_trial()
        self.assertEqual(sum(m == 'DELETE' for m, _ in self.client.mutations), 2)
        report = walk.load_json(self.root / 'en/report.json')
        self.assertFalse(report['passed'])
        self.assertTrue(report['acknowledgementStarted'])
        self.assertEqual(report['stageAtFailure'], 'cleanup-evidence-saved')
        self.assertTrue((self.root / 'en/after-cleanup.json').exists())

    def test_original_full_bytes_mismatch_before_upload(self):
        self.client.bad_original = True
        self.no_delete('fixture bytes differ')
        self.assertEqual(self.client.uploaded, 0)

    def test_backup_bad_run_prevents_upload(self):
        self.client.bad_backup_header = True
        self.no_delete('chunk binding mismatch')
        self.assertEqual(self.client.uploaded, 0)

    def test_backup_bad_hash_prevents_upload(self):
        self.client.bad_raw_hash = True
        self.no_delete('bytes changed')
        self.assertEqual(self.client.uploaded, 0)

    def test_recent_normalization_risk_prevents_upload(self):
        self.client.raw['recent'] = b'{"books":[{"path":"/x.epub","title":"","author":"","coverBmpPath":""}]}'
        self.no_delete('normalization')
        self.assertEqual(self.client.uploaded, 0)

    def test_raw_original_changed_keeps_frames_and_prevents_cleanup(self):
        self.client.bad_raw = True
        report = self.no_delete('original state change')
        self.assertFalse(report['rawProtectedStateUnchanged'])
        self.assertTrue((self.root / 'en/library-original.bmp').exists())
        self.assertTrue((self.root / 'en/raw-after-trial/state.bin').exists())

    def test_library_failure_keeps_all_available_evidence_without_cleanup(self):
        self.client.library_error = True
        self.no_delete('terminal evidence missing')
        self.assertTrue((self.root / 'en/final-page.bmp').exists())

    def test_interrupted_library_with_running_walk_is_saved_without_wait_or_cleanup(self):
        self.client.library_error = True
        self.client.result['state'] = 'running'
        self.no_delete('Walk did not complete')
        self.assertEqual(self.t, 3)
        self.assertEqual(walk.load_json(self.root / 'en/terminal-readiness.json')['walk']['state'], 'running')

    def test_missing_original_capture_not_promoted_from_owned_or_reader(self):
        self.client.missing_frame = True
        self.no_delete('terminal evidence missing')
        self.assertTrue((self.root / 'en/library-owned.bmp').exists())
        self.assertFalse((self.root / 'en/library-original.bmp').exists())

    def test_build_failure_preserved_before_no_cleanup(self):
        self.client.walk_error = failure_record()
        self.no_delete('Trial failure')
        self.assertEqual((self.root / 'en/walk-build-failure.bin').read_bytes(), failure_record())

    def test_public_reading_change_keeps_evidence(self):
        self.client.mutate_reading = True
        self.no_delete('original state change')

    def test_foreign_frame_binding_stops_without_cleanup(self):
        self.client.bad_capture_run = True
        self.no_delete('Capture run or size mismatch')
        self.assertTrue((self.root / 'en/terminal-readiness.json').exists())

    def test_unsafe_reference_and_plan_mutation_fail_offline(self):
        for path in ('/../book.epub', '/folder/book.epub', '/Pocket visual 123.epub', '/book.txt'):
            p = copy.deepcopy(self.plan); p['referencePath'] = path
            with self.assertRaises(walk.WalkError):
                lib.validate_plan(p)
        p = copy.deepcopy(self.plan); p['titleSHA256'] = 'A' * 64
        with self.assertRaises(walk.WalkError):
            lib.validate_plan(p)

    def test_invalid_manifest_flags_and_aggregate(self):
        for change in ('run', 'aggregate', 'size', 'absence'):
            m = self.client.manifest()
            if change == 'run': m['run'] = 124
            if change == 'aggregate': m['backupDigest'] = '0' * 64
            if change == 'size': m['files'][3]['bytes'] = 8 * 1024 * 1024 + 1
            if change == 'absence': m['files'][4]['sha256'] = '1' * 64
            with self.assertRaises(walk.WalkError): lib.validate_manifest(m, 123)

    def test_run_without_execute_has_zero_client_construction(self):
        with patch.object(lib, 'LibraryWalker') as runner:
            self.assertEqual(lib.main(['run', '--plan', str(self.root / 'plan.json')]), 2)
            runner.assert_not_called()


if __name__ == '__main__':
    unittest.main()
