"""Nonuniform wire samples, negative evidence and bounded-campaign tests."""
import argparse
import copy
from contextlib import nullcontext
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

import dev_reader_benchmark as campaign
import dev_reader_walk as walk
import reader_benchmark as perf
from test_dev_reader_walk import FakeClient, plan as walk_plan


def trace(run=123, pages=3, total=400):
    header = struct.pack('<7I', 0x4d424450, 1, run, pages, 48, 0, 0)
    data = header + struct.pack('<I', perf.checksum(header))
    for index in range(pages):
        row = struct.pack('<16HBBIIH', *list(range(index + 1, index + 12)), total + index,
                          7 + index, 91 + index, 5 + index, 101 + index,
                          1, 128 if index == 0 else 16, 90000 + index, 60000 + index, index)
        data += row + struct.pack('<I', perf.checksum(row))
    return data


class CodecTests(unittest.TestCase):
    def test_bw_work_sums_each_turn_before_percentiles_and_excludes_open_and_gray(self):
        rows = perf.decode(trace(pages=4), 123, 4)
        for row in rows:
            row['ms'] = dict.fromkeys(perf.STAGES, 0)
            row['ms'].update(grayRender=700, grayRefresh=800, graySync=900, save=500, total=3000)
        rows[0]['ms']['input'] = 60000
        rows[1]['ms']['prewarm'] = 100
        rows[2]['ms']['bwRefresh'] = 100
        rows[3]['ms']['input'] = 1
        result = perf.summary(rows)
        self.assertEqual(result['bwWorkP50Ms'], 100)
        self.assertEqual(result['bwWorkP95Ms'], 100)
        self.assertEqual(result['turnP50Ms'], 3000)
        self.assertEqual(result['counterP50'], dict(sdOpens=9, sdReads=93, sdKB=7, glyphs=103))

    def test_nonuniform_fields_and_order(self):
        rows = perf.decode(trace(), 123, 3)
        self.assertEqual(rows[2]['ms']['input'], 3)
        self.assertEqual(rows[2]['ms']['graySync'], 12)
        self.assertEqual(rows[2]['ms']['total'], 402)
        self.assertEqual(rows[1]['counters'], dict(sdOpens=8, sdReads=92, sdKB=6, glyphs=102))
        self.assertEqual(rows[2]['largestBlock'], 60002)
        self.assertEqual(perf.summary(rows)['turnP95Ms'], 402)

    def test_corruption_foreign_run_truncation_order_and_saturation(self):
        original = trace()
        broken = [original[:-1], original + b'x', original[:32] + original[80:128] + original[32:80] + original[128:]]
        for offset in (0, 8, 20, 28, 34, 42, 66, 76):
            data = bytearray(original)
            data[offset] ^= 1
            broken.append(data)
        saturated = bytearray(original)
        struct.pack_into('<H', saturated, 32 + 22, 65535)
        struct.pack_into('<I', saturated, 32 + 44, perf.checksum(saturated[32:76]))
        broken.append(saturated)
        wrong_index = bytearray(original)
        struct.pack_into('<H', wrong_index, 32 + 42, 1)
        struct.pack_into('<I', wrong_index, 32 + 44, perf.checksum(wrong_index[32:76]))
        broken.append(wrong_index)
        for data in broken:
            with self.subTest(data=data[:36]), self.assertRaises(ValueError):
                perf.decode(data, 123, 3)
        with self.assertRaises(ValueError):
            perf.decode(original, 124, 3)

    def test_paired_screen_rejects_regressions_and_insufficient_repeats(self):
        a, b = perf.decode(trace(total=400), 123, 3), perf.decode(trace(total=300), 123, 3)
        pairs = [(a, b)] * 5
        result = perf.assess(pairs)
        self.assertTrue(result['timingScreenPassed'])
        self.assertFalse(result['automaticAcceptance'])
        for field, value in [('freeHeap', 49999), ('largestBlock', 59000), ('flags', 0), ('strips', 0)]:
            bad = copy.deepcopy(b)
            bad[1][field] = value
            self.assertFalse(perf.assess(pairs[:4] + [(a, bad)])['timingScreenPassed'])
        bad = copy.deepcopy(b)
        bad[2]['ms']['total'] = 900
        self.assertFalse(perf.assess(pairs[:4] + [(a, bad)])['timingScreenPassed'])
        self.assertFalse(perf.assess([(a, a)] * 5)['timingScreenPassed'])
        with self.assertRaises(ValueError):
            perf.assess(pairs[:4])


class PerformanceClient(FakeClient):
    payload = trace()
    def get_json(self, path, query=None):
        value = super().get_json(path, query)
        if path == walk.API + 'dev/reader-capture':
            value.update(walkPerformanceVersion=1, maxPerformancePages=16)
        return value

    def request(self, method, path, query=None, **kwargs):
        if method == 'GET' and path == walk.API + 'dev/reader-capture' and query.get('performance') == 1:
            return self.payload, {'X-Reader-Performance-Run': str(self.fixture['run'])}
        return super().request(method, path, query, **kwargs)


class CollectionTests(unittest.TestCase):
    def test_trace_saved_before_cleanup_and_bad_trace_blocks_cleanup(self):
        for good in (True, False):
            with self.subTest(good=good), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                plan, data = walk_plan()
                walk.save_json(root / 'plan.json', plan)
                walk.save_bytes(root / 'en.epub', data)
                client = PerformanceClient(plan, data)
                client.before_delete = lambda: self.assertTrue((root / 'en/performance.bin').exists())
                walker = walk.Walker(root / 'plan.json', client, lambda: 1, lambda _: None)
                if good:
                    report = walker.execute_fixture(plan['fixtures'][0])
                    self.assertTrue(report['passed'])
                    self.assertEqual(len(report['performance']['records']), 3)
                else:
                    client.payload = trace(run=124)
                    with self.assertRaises(walk.WalkError):
                        walker.execute_fixture(plan['fixtures'][0])
                    self.assertFalse(any(method == 'DELETE' for method, _ in client.mutations))


class CampaignTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for label in ('baseline', 'candidate', 'restore'):
            (self.root / (label + '.bin')).write_bytes(b'CrossPoint version: 0.1.0-dev-' + label.encode() + b'\0')
        walk.save_json(self.root / 'config.json', {'host': '192.168.1.2', 'deviceID': 'TEST0001'})
        walk.save_json(self.root / 'backup.json', {'synthetic': True})
        (self.root / 'en.epub').write_bytes(b'PK unchanged test fixture')
        args = argparse.Namespace(pages=3, repeats=5, model='X3', output=self.root / 'campaign',
            config=self.root / 'config.json', backup_receipt=self.root / 'backup.json',
            fixture=['en=' + str(self.root / 'en.epub')],
            **{x: self.root / (x + '.bin') for x in ('baseline', 'candidate', 'restore')})
        self.path = campaign.plan(args)
        self.directory, self.manifest, _ = campaign.verified_plan(self.path)

    def test_plan_is_offline_alternating_complete_and_immutable(self):
        self.assertEqual([x['label'] for x in self.manifest['trials'][:4]],
                         ['baseline', 'candidate', 'candidate', 'baseline'])
        self.assertEqual(len(self.manifest['trials']), 10)
        (self.directory / 'candidate.bin').write_bytes(b'corrupt')
        with self.assertRaises(walk.WalkError):
            campaign.verified_plan(self.path)

    def populate(self):
        for trial in self.manifest['trials']:
            p = walk.load_json(self.directory / trial['directory'] / 'plan.json')
            fixture = p['fixtures'][0]
            folder = self.directory / trial['directory'] / 'en'
            rows = trace(fixture['run'], 3, 400 if trial['label'] == 'baseline' else 300)
            walk.save_bytes(folder / 'performance.bin', rows)
            walk.save_bytes(folder / 'final-page.bmp', b'synthetic-bmp')
            for name in ('before', 'after-cleanup'):
                walk.save_json(folder / (name + '.json'), {'preferences': {'fontSize': 1}})
            terminal = FakeClient(*walk_plan()).result
            terminal['run'] = fixture['run']
            walk.save_json(folder / 'report.json', {'passed': True, 'stage': 'complete',
                'publicStateUnchanged': True, 'ownedFileCleaned': True, 'sourceBytesVerified': True,
                'run': fixture['run'], 'sourceSHA256': fixture['sha256'], 'walk': terminal,
                'performance': {'sha256': walk.digest(rows)}, 'capture': {'sha256': walk.digest(b'synthetic-bmp')}})

    def test_complete_evidence_required_and_final_output_must_match(self):
        self.populate()
        self.assertTrue(campaign.compare(self.path)['timingScreenPassed'])
        folder = self.directory / '00-candidate/en'
        report = walk.load_json(folder / 'report.json')
        report['capture']['sha256'] = walk.digest(b'changed')
        walk.save_bytes(folder / 'final-page.bmp', b'changed')
        walk.save_json(folder / 'report.json', report)
        with self.assertRaises(walk.WalkError):
            campaign.compare(self.path)

    def test_failed_walk_stops_before_restore_or_next_flash(self):
        original = FakeClient(*walk_plan()).status
        original['version'] = '0.1.0-dev-restore'
        current = dict(original)
        def install(command, **kwargs):
            current['version'] = '0.1.0-dev-' + Path(command[2]).stem
        with patch.object(walk.Client, 'get_json', side_effect=lambda *_: current), \
             patch.object(walk, 'readiness', return_value={'walkPerformanceVersion': 1, 'maxPerformancePages': 16}), \
             patch.object(walk, 'fresh_readiness'), patch.object(campaign.dev_pipeline, 'require_companion_stopped'), \
             patch.object(campaign.dev_pipeline, 'exclusive', return_value=nullcontext()), \
             patch.object(walk, 'snapshot', return_value={'synthetic': True}), \
             patch.object(walk.Walker, 'execute_fixture', side_effect=walk.WalkError('synthetic failure')), \
             patch.object(campaign.subprocess, 'run', side_effect=install) as flash:
            with self.assertRaises(walk.WalkError):
                campaign.execute(self.path)
            self.assertEqual(flash.call_count, 1)
        report = walk.load_json(self.directory / 'execution.json')
        self.assertEqual(report['state'], 'stopped')
        self.assertFalse(report['restorationVerified'])
        with self.assertRaises(walk.WalkError):
            campaign.execute(self.path)

    def test_clean_campaign_restores_even_when_timing_screen_fails(self):
        current = FakeClient(*walk_plan()).status
        current['version'] = '0.1.0-dev-restore'
        def install(command, **kwargs):
            current['version'] = '0.1.0-dev-' + Path(command[2]).stem
        with patch.object(walk.Client, 'get_json', side_effect=lambda *_: current), \
             patch.object(walk, 'readiness', return_value={'walkPerformanceVersion': 1, 'maxPerformancePages': 16}), \
             patch.object(walk, 'fresh_readiness'), patch.object(campaign.dev_pipeline, 'require_companion_stopped'), \
             patch.object(campaign.dev_pipeline, 'exclusive', return_value=nullcontext()), \
             patch.object(walk, 'snapshot', return_value={'synthetic': True}), \
             patch.object(walk.Walker, 'execute_fixture') as execute, \
             patch.object(campaign, 'compare', return_value={'timingScreenPassed': False}), \
             patch.object(campaign.subprocess, 'run', side_effect=install) as flash:
            result = campaign.execute(self.path)
        self.assertEqual(execute.call_count, 10)
        self.assertEqual(Path(flash.call_args.args[0][2]).name, 'restore.bin')
        self.assertTrue(result['restorationVerified'])
        self.assertFalse(result['assessment']['timingScreenPassed'])


if __name__ == '__main__':
    unittest.main()
