#!/usr/bin/env python3
"""Offline A/B planning and bounded developer-device reader experiments.

Freeze both instrumented BINs, the original restore BIN, fixtures and backup
receipt with `plan`. `run --execute` alternates AB/BA pairs, stops on the first
failure, retains all evidence and restores the original firmware after clean
trials. It never commits, publishes, auto-accepts, or retries an ambiguous trial.
"""
import argparse
from contextlib import redirect_stdout
import json
import os
from pathlib import Path
import subprocess
import sys

import dev_pipeline
import dev_reader_walk as walk
import pocket_put
import reader_benchmark as perf

ROOT = Path(__file__).resolve().parents[1]


def freeze(source, target):
    data = Path(source).read_bytes()
    walk.save_bytes(target, data)
    return {'file': target.name, 'sha256': walk.digest(data), 'bytes': len(data)}


def plan(args):
    walk.require(2 <= args.pages <= perf.MAX_PAGES, 'Benchmark pages must be 2..16')
    walk.require(5 <= args.repeats <= 10, 'Benchmark requires 5..10 paired walks per fixture')
    walk.require(not args.output.exists(), 'Choose a fresh output directory')
    root = args.output.resolve()
    root.mkdir(parents=True, mode=0o700)
    os.chmod(root, 0o700)
    manifest = {'schema': 'pocket-reader-benchmark-v1', 'model': args.model,
                'pages': args.pages, 'repeats': args.repeats, 'minimumGain': .10,
                'artifacts': {}, 'trials': [], 'automaticAcceptance': False,
                'scope': 'Cold path cache, forward turns with 1 s dwell, dev firmware, final BW only. No optical or energy proof.'}
    for label in ('baseline', 'candidate', 'restore'):
        item = freeze(getattr(args, label), root / (label + '.bin'))
        item['version'] = pocket_put.developer_version((root / item['file']).read_bytes())
        manifest['artifacts'][label] = item
    walk.require(manifest['artifacts']['baseline']['version'] != manifest['artifacts']['candidate']['version'],
                 'A/B images need distinct exact developer versions')
    walk.require(manifest['artifacts']['baseline']['sha256'] != manifest['artifacts']['candidate']['sha256'],
                 'A/B images must differ')
    versions = {}
    for item in manifest['artifacts'].values():
        walk.require(versions.get(item['version'], item['sha256']) == item['sha256'],
                     'A version must identify only one firmware artifact')
        versions[item['version']] = item['sha256']
    backup = json.loads(args.backup_receipt.read_bytes())
    walk.require(isinstance(backup, dict) and bool(backup), 'Existing model-specific backup receipt required')
    manifest['backupReceipt'] = freeze(args.backup_receipt, root / 'backup-receipt.json')
    manifest['config'] = freeze(args.config, root / 'config.json')
    # Each repetition has its own immutable run ID and cold path-keyed cache.
    for repeat in range(args.repeats):
        for label in (('baseline', 'candidate') if repeat % 2 == 0 else ('candidate', 'baseline')):
            directory = f'{repeat:02d}-{label}'
            path = walk.create_plan(argparse.Namespace(config=root / 'config.json',
                expected_version=manifest['artifacts'][label]['version'], model=args.model,
                pages=args.pages, fixture=args.fixture, output=root / directory))
            manifest['trials'].append({'repeat': repeat, 'label': label, 'directory': directory,
                                      'planSHA256': walk.digest(Path(path).read_bytes())})
    walk.save_json(root / 'plan.json', manifest)
    return root / 'plan.json'


def verified_plan(path):
    root, manifest = path.resolve().parent, walk.load_json(path)
    walk.require(manifest.get('schema') == 'pocket-reader-benchmark-v1', 'Unknown benchmark schema')
    walk.require(manifest.get('model') in ('X3', 'X4') and type(manifest.get('pages')) is int
                 and 2 <= manifest['pages'] <= perf.MAX_PAGES, 'Invalid model/pages')
    repeats = manifest.get('repeats')
    walk.require(type(repeats) is int and 5 <= repeats <= 10, 'Invalid repeat count')
    walk.require(manifest.get('minimumGain') == .10, 'Benchmark threshold changed')
    for name, item in {**manifest['artifacts'], 'config': manifest['config'],
                       'backup-receipt': manifest['backupReceipt']}.items():
        filename = name + ('.bin' if name in ('baseline', 'candidate', 'restore') else '.json')
        walk.require(item['file'] == filename, 'Unsafe frozen artifact path')
        data = (root / filename).read_bytes()
        walk.require(len(data) == item['bytes'] and walk.digest(data) == item['sha256'], 'Frozen input changed')
        if filename.endswith('.bin'):
            walk.require(pocket_put.developer_version(data) == item['version'], 'Artifact version changed')
    artifacts = manifest['artifacts']
    walk.require(set(artifacts) == {'baseline', 'candidate', 'restore'} and
                 artifacts['baseline']['version'] != artifacts['candidate']['version'] and
                 artifacts['baseline']['sha256'] != artifacts['candidate']['sha256'], 'Invalid A/B artifacts')
    versions = {}
    for item in artifacts.values():
        walk.require(versions.get(item['version'], item['sha256']) == item['sha256'],
                     'Ambiguous firmware version')
        versions[item['version']] = item['sha256']
    expected = [(i, label) for i in range(repeats) for label in
                (('baseline', 'candidate') if i % 2 == 0 else ('candidate', 'baseline'))]
    walk.require([(x['repeat'], x['label']) for x in manifest['trials']] == expected, 'Incomplete or reordered campaign')
    config = walk.load_json(root / 'config.json')
    fixtures, runs = None, set()
    for trial in manifest['trials']:
        walk.require(trial['directory'] == f"{trial['repeat']:02d}-{trial['label']}", 'Unsafe trial path')
        source = root / trial['directory'] / 'plan.json'
        walk.require(walk.digest(source.read_bytes()) == trial['planSHA256'], 'Frozen walk plan changed')
        walker = walk.Walker(source)
        p = walker.plan
        walk.require(p['host'] == config['host'] and p['deviceID'] == config['deviceID'] and
                     p['model'] == manifest['model'] and p['pages'] == manifest['pages'] and
                     p['expectedVersion'] == artifacts[trial['label']]['version'], 'Walk identity changed')
        signature = [(x['label'], x['sha256'], x['bytes']) for x in p['fixtures']]
        fixtures = signature if fixtures is None else fixtures
        walk.require(fixtures == signature, 'Fixtures changed between trials')
        for fixture in p['fixtures']:
            walk.require(fixture['run'] not in runs, 'Repeated run ID')
            runs.add(fixture['run'])
    return root, manifest, config


def compare(path):
    root, manifest, _ = verified_plan(path)
    evidence, initial = {}, None
    for trial in manifest['trials']:
        folder = root / trial['directory']
        p = walk.load_json(folder / 'plan.json')
        for fixture in p['fixtures']:
            run = folder / fixture['label']
            report = walk.load_json(run / 'report.json')
            walk.require(report.get('passed') is True and report.get('stage') == 'complete' and
                         report.get('publicStateUnchanged') is True and report.get('ownedFileCleaned') is True and
                         report.get('sourceBytesVerified') is True and report.get('run') == fixture['run'] and
                         report.get('sourceSHA256') == fixture['sha256'], 'Incomplete/failed walk; no sample may be dropped')
            walk.validate_result(report['walk'], fixture, manifest['pages'])
            before = walk.load_json(run / 'before.json')
            initial = before if initial is None else initial
            walk.require(before == initial == walk.load_json(run / 'after-cleanup.json'), 'Settings/state drift between trials')
            raw = (run / 'performance.bin').read_bytes()
            walk.require(walk.digest(raw) == report['performance']['sha256'], 'Performance evidence changed')
            records = perf.decode(raw, fixture['run'], manifest['pages'])
            bmp = (run / 'final-page.bmp').read_bytes()
            walk.require(walk.digest(bmp) == report['capture']['sha256'], 'Capture evidence changed')
            final = report['walk']
            evidence[(trial['repeat'], trial['label'], fixture['label'])] = (
                records, walk.digest(bmp), (final['lastSpine'], final['lastPage'], final['progressPercent']))
    results = {}
    for label in {key[2] for key in evidence}:
        pairs = []
        for repeat in range(manifest['repeats']):
            a, b = (evidence[(repeat, side, label)] for side in ('baseline', 'candidate'))
            walk.require(a[1:] == b[1:], 'Final page/output changed; candidate rejected')
            pairs.append((a[0], b[0]))
        results[label] = perf.assess(pairs, manifest['minimumGain'])
    return {'fixtures': results, 'timingScreenPassed': all(x['timingScreenPassed'] for x in results.values()),
            'automaticAcceptance': False, 'productionAcceptance': False}


def execute(path):
    root, manifest, config = verified_plan(path)
    journal = root / 'execution.json'
    walk.require(not journal.exists(), 'Campaign already started; inspect evidence, never replay run')
    client = walk.Client(config)
    result = {'state': 'preflight', 'passed': False, 'restorationVerified': False,
              'planSHA256': walk.digest(path.read_bytes()), 'automaticAcceptance': False}

    def status():
        current = client.get_json('/api/status')
        walk.require(current.get('deviceID') == config['deviceID'] and current.get('device') == manifest['model'],
                     'Enrolled model/identity mismatch')
        walk.require(isinstance(current.get('version'), str) and '-dev-' in current['version'],
                     'An identified developer build is required before installation')
        walk.verify_status(current, dict(config, model=manifest['model'], expectedVersion=current.get('version', '')))
        return current

    def install(label):
        item = manifest['artifacts'][label]
        walk.require(walk.digest((root / item['file']).read_bytes()) == item['sha256'], 'Frozen firmware changed')
        current = status()
        if current.get('version') != item['version']:
            result.update(state='install-submitted', installing=label)
            walk.save_json(journal, result)
            # Existing uploader verifies staged bytes, observes a lost flash
            # reply and never reissues the flash POST. One upload attempt only.
            fd = os.open(root / 'install-private.log', os.O_CREAT | os.O_WRONLY | os.O_APPEND, 0o600)
            with os.fdopen(fd, 'a') as log:
                subprocess.run([sys.executable, str(ROOT / 'scripts/pocket_put.py'), str(root / item['file']),
                                '--host', config['host'], '--target', 'update.bin', '--attempts', '1', '--dev-flash'],
                               check=True, timeout=600, stdout=log, stderr=subprocess.STDOUT)
        walk.require(status().get('version') == item['version'], 'Installed version not verified')

    with dev_pipeline.exclusive(ROOT / 'build/dev-pipeline.lock'):
        dev_pipeline.require_companion_stopped()
        walk.require(status().get('version') == manifest['artifacts']['restore']['version'],
                     'Restore artifact must match the original installed developer version')
        walk.fresh_readiness(walk.readiness(client, 1), 1)
        original_plan = dict(config, model=manifest['model'], expectedVersion=manifest['artifacts']['restore']['version'])
        original_state = walk.snapshot(client, original_plan)
        walk.save_json(root / 'original-state.json', original_state)
        walk.save_json(journal, result)
        try:
            for trial in manifest['trials']:
                # Do not flash over an unreviewed operation/receipt.
                walk.fresh_readiness(walk.readiness(client, 1), 1)
                install(trial['label'])
                ready = walk.readiness(client, 1)
                walk.require(ready.get('walkPerformanceVersion') == 1 and ready.get('maxPerformancePages') == 16,
                             'Instrumented firmware required on both sides')
                walker = walk.Walker(root / trial['directory'] / 'plan.json')
                for fixture in walker.plan['fixtures']:
                    result.update(state='walk-submitted', trial=trial['directory'], fixture=fixture['label'])
                    walk.save_json(journal, result)
                    walker.execute_fixture(fixture)
            # Restore after clean trial cleanup, even if the speed screen fails.
            walk.fresh_readiness(walk.readiness(client, 1), 1)
            install('restore')
            restored_state = walk.snapshot(client, original_plan)
            walk.save_json(root / 'restored-state.json', restored_state)
            walk.require(restored_state == original_state, 'Original public state differs after firmware restoration')
            result.update(state='restored', restorationVerified=True, fullSDRestoreVerified=False,
                          installedVersionVerified=True, originalPublicStateVerified=True)
            walk.save_json(journal, result)
            result['assessment'] = compare(path)
            result.update(state='complete', executionPassed=True, passed=result['assessment']['timingScreenPassed'])
        except BaseException as error:
            result.update(failedAt=result['state'], state='stopped', errorType=type(error).__name__,
                          recovery='Preserve receipts and pending trials. Observe/repair before any new flash or run.')
            raise
        finally:
            walk.save_json(journal, result)
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    p = sub.add_parser('plan')
    for name in ('config', 'baseline', 'candidate', 'restore', 'backup-receipt', 'output'):
        p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--model', choices=('X3', 'X4'), required=True)
    p.add_argument('--pages', type=int, default=16)
    p.add_argument('--repeats', type=int, default=5)
    p.add_argument('--fixture', action='append', required=True)
    for action in ('run', 'compare'):
        p = sub.add_parser(action)
        p.add_argument('--plan', type=Path, required=True)
        if action == 'run':
            p.add_argument('--execute', action='store_true')
    args = parser.parse_args(argv)
    try:
        if args.action == 'plan':
            print(plan(args))
        elif args.action == 'compare':
            print(json.dumps(compare(args.plan), indent=2))
        else:
            walk.require(args.execute, 'Use --execute for the authorized device campaign')
            root = args.plan.resolve().parent
            fd = os.open(root / 'execution-private.log', os.O_CREAT | os.O_WRONLY | os.O_APPEND, 0o600)
            with os.fdopen(fd, 'a') as stream, redirect_stdout(stream):
                execute(args.plan)
            print('Campaign complete; review execution.json (no automatic acceptance)')
        return 0
    except (OSError, ValueError, KeyError, walk.WalkError, pocket_put.ProtocolError, subprocess.SubprocessError) as error:
        print(f'Benchmark stopped ({type(error).__name__}); inspect private evidence; no mutation replayed', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
