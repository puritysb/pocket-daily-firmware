#!/usr/bin/env python3
"""Developer Library entry trial. Plan is offline; run/observe require explicit I/O opt-in.

Captures the actual original and owned-copy Library rows, then opens only the
owned copy through Library selection. All raw backups stay private. A reported
protocol pass still requires independent captured-frame review.
"""
import argparse
from contextlib import redirect_stdout
import hashlib
import io
import json
import os
from pathlib import Path
import re
import struct
import sys
import time
import urllib.error
import xml.etree.ElementTree as ET
import zipfile
import zlib

import dev_reader_walk as walk

API = walk.API + 'dev/reader-capture'
SELECTORS = ('state', 'recent', 'settings', 'index', 'dirty')
CAPS = {'state': 65536, 'recent': 65536, 'settings': 65536, 'index': 8 * 1024 * 1024, 'dirty': 1024}
require = walk.require


def epub_title(data):
    with zipfile.ZipFile(io.BytesIO(data)) as book:
        container = ET.fromstring(book.read('META-INF/container.xml'))
        roots = container.findall('.//{*}rootfile')
        require(len(roots) == 1, 'Exactly one EPUB package is required')
        name = roots[0].get('full-path', '')
        require(name and not name.startswith('/') and '..' not in name.split('/'), 'Unsafe EPUB package path')
        entry = book.getinfo(name)
        require(entry.file_size <= 65536, 'EPUB metadata exceeds bounded size')
        package = ET.fromstring(book.read(name))
        titles = package.findall('.//{http://purl.org/dc/elements/1.1/}title')
        require(len(titles) == 1 and titles[0].text, 'Exactly one plain EPUB title is required')
        title = titles[0].text.strip()
        require(0 < len(title.encode()) < 256 and '\ufffd' not in title, 'Invalid EPUB title')
        return title


def validate_plan(plan):
    walk.valid_plan(plan)
    require(plan.get('libraryTrialVersion') == 1 and len(plan['fixtures']) == 1,
            'One explicit Library trial is required')
    reference = plan.get('referencePath', '')
    require(re.fullmatch(r'/[A-Za-z0-9_.-]+\.epub', reference) and '..' not in reference and len(reference) < 128,
            'Reference must be one exact ASCII root EPUB path')
    require(re.fullmatch(r'[a-f0-9]{64}', plan.get('titleSHA256', '')), 'Expected title digest required')


def create_plan(args):
    require(len(args.fixture) == 1, 'One fixture per Library trial')
    path = walk.create_plan(args)
    plan = walk.load_json(path)
    data = (path.parent / plan['fixtures'][0]['local']).read_bytes()
    plan.update(libraryTrialVersion=1, referencePath=args.reference_path,
                titleSHA256=walk.digest(epub_title(data).encode()),
                scope='Original Library row capture, owned row capture, actual owned selection and isolated reader walk.',
                rawBackupScope=list(SELECTORS),
                derivedIndexPolicy='Backed up and allowed to rebuild; no owned row may remain after cleanup.',
                screenValidation='Independent frame review required; counters do not prove title visibility or pixels.')
    validate_plan(plan)
    walk.save_json(path, plan)
    return path


def validate_manifest(manifest, run):
    require(type(manifest.get('version')) is int and manifest['version'] == 1 and manifest.get('run') == run,
            'Backup version/run mismatch')
    rows = manifest.get('files')
    require(isinstance(rows, list) and len(rows) == len(SELECTORS), 'Known backup selectors required')
    for name, row in zip(SELECTORS, rows):
        require(isinstance(row, dict) and row.get('selector') == name and type(row.get('present')) is bool
                and type(row.get('bytes')) is int and 0 <= row['bytes'] <= CAPS[name]
                and re.fullmatch(r'[a-f0-9]{64}', row.get('sha256', '')), 'Invalid bounded backup manifest')
        require(row['present'] or (row['bytes'] == 0 and row['sha256'] == '0' * 64), 'Invalid absent backup')
    joined = b''.join(struct.pack('<BBI', i, row['present'], row['bytes']) + bytes.fromhex(row['sha256'])
                      for i, row in enumerate(rows[:4]))
    require(manifest.get('backupDigest') == walk.digest(joined), 'Backup aggregate digest mismatch')
    return rows


def backup(client, run, folder):
    folder.mkdir(mode=0o700)
    query = {'run': run, 'libraryBackup': 'manifest'}
    manifest = client.get_json(API, query)
    walk.save_json(folder / 'manifest.json', manifest)
    rows = validate_manifest(manifest, run)
    for row in rows:
        if not row['present']:
            continue
        content = bytearray()
        while len(content) < row['bytes']:
            offset = len(content)
            part, headers = client.request('GET', API, {'run': run, 'libraryBackup': row['selector'], 'offset': offset}, limit=1024)
            headers = {key.lower(): value for key, value in headers.items()}
            require(headers.get('x-backup-run') == str(run) and headers.get('x-backup-total') == str(row['bytes'])
                    and headers.get('x-backup-offset') == str(offset), 'Backup chunk binding mismatch')
            require(len(part) == min(1024, row['bytes'] - offset), 'Incomplete backup chunk')
            content.extend(part)
        require(walk.digest(content) == row['sha256'], 'Backup bytes changed during read')
        walk.save_bytes(folder / (row['selector'] + '.bin'), content)
    require(client.get_json(API, query) == manifest, 'Backup changed during snapshot; no mutation allowed')
    return manifest


def canonical_recent(folder):
    path = folder / 'recent.bin'
    if not path.exists():
        return
    value = json.loads(path.read_bytes())
    require(isinstance(value, dict) and isinstance(value.get('books'), list) and len(value['books']) <= 10,
            'Recent metadata is not canonical; refuse boot migration or sanitization')
    paths = set()
    for item in value['books']:
        require(isinstance(item, dict) and all(isinstance(item.get(k), str) for k in ('path', 'title', 'author', 'coverBmpPath'))
                and item['path'].startswith('/') and item['path'] not in paths and item['title']
                and '\ufffd' not in item['title'] and '\ufffd' not in item['author'],
                'Recent metadata would require normalization; no trial allowed')
        paths.add(item['path'])


def protected_equal(before, after):
    return before['files'][:3] == after['files'][:3]


def validate_library(state, fixture, phase):
    require(type(state.get('libraryTrialVersion')) is int and state['libraryTrialVersion'] == 1,
            'Library trial protocol unavailable')
    record = state.get('library')
    require(isinstance(record, dict) and type(record.get('version')) is int and record['version'] == 1 and record.get('present') is True
            and record.get('valid') is True and record.get('run') == fixture['run'], 'Library receipt invalid or foreign')
    require(all(type(record.get(k)) is int for k in ('run', 'phase', 'error', 'flags', 'openCount', 'originalRow', 'ownedRow', 'elapsedMs')),
            'Library counters must be integers')
    require(record['phase'] == phase and record['error'] == 0 and record['flags'] == 15 and record['openCount'] == 1
            and 0 <= record['originalRow'] < 4106 and 0 <= record['ownedRow'] < 4106
            and 0 <= record['elapsedMs'] <= 600000 and record.get('originalCapture') is True
            and record.get('ownedCapture') is True and record.get('derivedIndexMayChange') is True,
            'Library row, frame, one-shot open or terminal evidence missing')
    return record


def capture_frame(client, fixture, model, frame=None):
    if frame is None:
        return walk.download_capture(client, fixture, model)
    class Adapter:
        def request(self, method, path, query=None, **kwargs):
            return client.request(method, path, dict(query or {}, frame=frame), **kwargs)
    return walk.download_capture(Adapter(), fixture, model)


class LibraryWalker(walk.Walker):
    def __init__(self, plan_path, **kwargs):
        super().__init__(plan_path, **kwargs)
        validate_plan(self.plan)
        require(walk.digest(epub_title((self.root / self.plan['fixtures'][0]['local']).read_bytes()).encode())
                == self.plan['titleSHA256'], 'Frozen EPUB title changed')

    def observe(self, fixture, folder, report):
        require(report.get('submitted') is True, 'No durable one-shot submission marker')
        deadline = self.clock() + 720
        while self.clock() < deadline:
            self.sleep(3)
            try:
                walk.verify_status(self.client.get_json('/api/status'), self.plan)
                state = walk.readiness(self.client, fixture['run'])
            except (OSError, urllib.error.URLError):
                continue
            except walk.HTTPFailure as error:
                if error.code in (409, 503):
                    continue
                raise
            record = state.get('library')
            terminal = (state['walk'].get('state') in walk.TERMINAL or
                        isinstance(record, dict) and record.get('present') is True and
                        (record.get('valid') is not True or record.get('phase') in (6, 7, 8, 9)))
            if not terminal:
                require(state['walk'].get('run') in (0, fixture['run']), 'Foreign walk receipt')
                continue
            # A reboot-interrupted Library phase may retain Walk=Running. Save
            # that exact checked failure immediately, without inventing completion.
            path = folder / 'terminal-readiness.json'
            walk.save_json(path, state)
            report.update(terminalReadiness={'file': path.name, 'bytes': path.stat().st_size,
                          'sha256': walk.digest(path.read_bytes())}, walk=state['walk'],
                          library=record, imageValidation='not-performed', paintValidation='not-performed',
                          imageVerificationPending=True, paintVerificationPending=True)
            self.persist(folder, report)
            require(isinstance(record, dict) and record.get('valid') is True and record.get('run') == fixture['run'],
                    'Library receipt invalid or foreign; raw evidence retained')
            return state
        raise walk.WalkError('No terminal Library receipt within bounded observation; POST was not repeated')

    def reference(self, fixture):
        return dict(fixture, target=self.plan['referencePath'])

    def finish(self, fixture, folder, report):
        state = self.observe(fixture, folder, report)
        report['library'] = state.get('library')
        # Preserve all available failure evidence before enforcing success or cleanup.
        record, raw = walk.diagnostic(self.client, fixture['run'])
        report['walkBuildFailure'] = record
        if raw is not None:
            walk.save_bytes(folder / 'walk-build-failure.bin', raw)
        library = state.get('library') if isinstance(state.get('library'), dict) else {}
        for flag, name in (('originalCapture', 'library-original'), ('ownedCapture', 'library-owned')):
            if library.get(flag) is True:
                data, info = capture_frame(self.client, fixture, self.plan['model'], name)
                walk.save_bytes(folder / (name + '.bmp'), data)
                report[name] = info
        if state['walk'].get('captureReady') is True:
            data, report['capture'] = capture_frame(self.client, fixture, self.plan['model'])
            walk.save_bytes(folder / 'final-page.bmp', data)
        after = walk.snapshot(self.client, self.plan)
        walk.save_json(folder / 'after-trial.json', after)
        after_raw = backup(self.client, fixture['run'], folder / 'raw-after-trial')
        before = walk.load_json(folder / 'before.json')
        before_raw = walk.load_json(folder / 'raw-before/manifest.json')
        original = dict(after, inventory=[item for item in after['inventory'] if '/' + item['name'] != fixture['target']])
        report.update(stage='terminal-evidence-saved', rawProtectedStateUnchanged=protected_equal(before_raw, after_raw),
                      publicStateUnchangedBeforeCleanup=original == before, libraryFrameValidation='not-performed',
                      libraryFrameReviewPending=True)
        self.persist(folder, report)
        report['imageDiagnosticsValidation'] = walk.validate_image_diagnostics(state, fixture)
        report['paintDiagnosticsValidation'] = walk.validate_paint_diagnostics(state, fixture)
        walk.validate_result(state['walk'], fixture, self.plan['pages'])
        validate_library(state, fixture, 6)
        require(record is None and original == before and protected_equal(before_raw, after_raw),
                'Trial failure or original state change; all recovery artifacts retained')
        walk.download_book(self.client, fixture)
        walk.download_book(self.client, self.reference(fixture))
        report.update(stage='evidence-saved-before-cleanup', sourceBytesVerified=True, originalBytesVerified=True,
                      cleanupStarted=True)
        self.persist(folder, report)
        walk.verify_status(self.client.get_json('/api/status'), self.plan)
        # The only cleanup request removes the owned book/cache and rebuilds the
        # derived index. A lost reply is read back, never replayed.
        try:
            self.client.request('DELETE', API, {'run': fixture['run']}, expected=204)
        except (OSError, urllib.error.URLError):
            report['cleanupReplyLost'] = True
            self.persist(folder, report)
        deadline = self.clock() + 90
        cleaned = None
        while self.clock() < deadline:
            self.sleep(2)
            try:
                walk.verify_status(self.client.get_json('/api/status'), self.plan)
                current = walk.readiness(self.client, fixture['run'])
            except (OSError, urllib.error.URLError):
                continue
            except walk.HTTPFailure as error:
                if error.code in (409, 503):
                    continue
                raise
            walk.save_json(folder / 'cleanup-readiness.json', current)
            r = current.get('library', {})
            require(r.get('run') == fixture['run'] and r.get('valid') is True, 'Cleanup receipt invalid or foreign')
            if r.get('phase') == 9:
                cleaned = current
                break
            require(r.get('phase') == 8, 'Unexpected cleanup phase; no DELETE replay')
        require(cleaned is not None, 'No completed cleanup receipt; preserve evidence and do not retry deletion')
        validate_library(cleaned, fixture, 9)
        final_raw = backup(self.client, fixture['run'], folder / 'raw-after-cleanup')
        final = walk.snapshot(self.client, self.plan)
        walk.save_json(folder / 'after-cleanup.json', final)
        require(final == before and protected_equal(before_raw, final_raw), 'Original state changed after cleanup')
        walk.download_book(self.client, self.reference(fixture))
        report.update(stage='cleanup-evidence-saved', ownedFileCleaned=True, derivedIndexRebuilt=True,
                      noOwnedIndexRow=True, noOwnedIndexRowEvidence='Checked firmware LibraryIndexFile scan and saved raw index',
                      publicStateUnchanged=True, rawProtectedStateUnchanged=True,
                      derivedIndexByteIdentical=before_raw['files'][3] == final_raw['files'][3], cleanup=cleaned['library'])
        self.persist(folder, report)
        # A separate one-shot ACK is allowed only after all evidence is durable.
        report['acknowledgementStarted'] = True
        self.persist(folder, report)
        self.client.request('DELETE', API, {'run': fixture['run'], 'libraryAck': 'true'}, expected=204)
        fresh = walk.readiness(self.client, fixture['run'])
        walk.fresh_readiness(fresh, fixture['run'])
        require(fresh.get('library', {}).get('present') is False, 'Library acknowledgement left an artifact')
        walk.save_json(folder / 'after-acknowledgement.json', fresh)
        report.update(stage='complete', passed=True, protocolAcceptance=True, libraryFrameValidation='not-performed',
                      fullSDRestoreVerified=False, productionAcceptance=False, opticalAcceptance=False)
        self.persist(folder, report)
        return report

    def execute_fixture(self, fixture):
        folder = self.root / fixture['label']
        require(not folder.exists(), 'Trial already started; never replay run or cleanup')
        folder.mkdir(mode=0o700)
        report = {'run': fixture['run'], 'passed': False, 'stage': 'preflight', 'submitted': False,
                  'requestedPages': self.plan['pages'], 'sourceSHA256': fixture['sha256'],
                  'productionAcceptance': False, 'libraryFrameValidation': 'not-performed'}
        self.persist(folder, report)
        try:
            status = walk.verify_status(self.client.get_json('/api/status'), self.plan)
            ready = walk.readiness(self.client, fixture['run'])
            walk.fresh_readiness(ready, fixture['run'])
            require(ready.get('libraryTrialVersion') == 1 and ready.get('library', {}).get('present') is False,
                    'Library-capable fresh developer firmware required')
            before = walk.snapshot(self.client, self.plan)
            require(not any('/' + item['name'] == fixture['target'] for item in before['inventory']), 'Owned path conflict')
            walk.save_json(folder / 'before.json', before)
            walk.download_book(self.client, self.reference(fixture))
            raw = backup(self.client, fixture['run'], folder / 'raw-before')
            canonical_recent(folder / 'raw-before')
            report['stage'] = 'raw-backup-saved-before-upload'
            self.persist(folder, report)
            data = (self.root / fixture['local']).read_bytes()
            self.client.upload(status, data, fixture['staging'])
            report['stage'] = 'commit-submission-prepared'
            self.persist(folder, report)
            try:
                self.client.request('POST', walk.API + 'commit', body={'staging': fixture['staging'], 'target': fixture['target'],
                                    'size': len(data), 'crc32': f'{zlib.crc32(data):08X}'})
            except (OSError, urllib.error.URLError):
                report['commitReplyLost'] = True
            walk.verify_status(self.client.get_json('/api/status'), self.plan)
            walk.download_book(self.client, fixture)
            current = self.client.get_json(API, {'run': fixture['run'], 'libraryBackup': 'manifest'})
            validate_manifest(current, fixture['run'])
            require(current['files'][:4] == raw['files'][:4], 'Raw original state or index changed before trial')
            ready = walk.readiness(self.client, fixture['run'])
            require(ready.get('bookReady') is True, 'Published owned book is unavailable')
            walk.fresh_readiness(dict(ready, bookReady=False), fixture['run'])
            report.update(stage='walk-submission-prepared', submitted=True)
            self.persist(folder, report)
            try:
                self.client.request('POST', API, {'run': fixture['run'], 'action': 'library-walk'},
                                    body={'pages': self.plan['pages'], 'bytes': fixture['bytes'],
                                          'referencePath': self.plan['referencePath'], 'sourceSHA256': fixture['sha256'],
                                          'titleSHA256': self.plan['titleSHA256'], 'backupDigest': raw['backupDigest'],
                                          'backupAcknowledged': True}, expected=202)
            except (OSError, urllib.error.URLError):
                report['walkReplyLost'] = True
            self.persist(folder, report)
            return self.finish(fixture, folder, report)
        except BaseException as error:
            report.update(passed=False, stageAtFailure=report['stage'], errorType=type(error).__name__,
                          error=str(error) if isinstance(error, walk.WalkError) else 'Trial stopped; private evidence retained',
                          cleanupMayBePartial=report.get('cleanupStarted', False), remainingRemoteArtifactsPreserved=True,
                          localRecoveryEvidencePreserved=True)
            self.persist(folder, report)
            raise


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    plan = sub.add_parser('plan')
    for name in ('config', 'output'):
        plan.add_argument('--' + name, type=Path, required=True)
    plan.add_argument('--expected-version', required=True)
    plan.add_argument('--model', choices=('X3', 'X4'), required=True)
    plan.add_argument('--fixture', action='append', required=True)
    plan.add_argument('--reference-path', required=True)
    plan.add_argument('--pages', type=int, default=1)
    for name in ('run', 'observe'):
        command = sub.add_parser(name)
        command.add_argument('--plan', type=Path, required=True)
        command.add_argument('--execute', action='store_true')
        if name == 'run':
            command.add_argument('--backup-confirmed', action='store_true')
            command.add_argument('--no-unrecovered-transfers', action='store_true')
    args = parser.parse_args(argv)
    try:
        if args.action == 'plan':
            print(create_plan(args))
            return 0
        require(args.execute, 'Device I/O requires --execute; plan is offline')
        runner = LibraryWalker(args.plan)
        with walk.dev_pipeline.exclusive(walk.ROOT / 'build/dev-pipeline.lock'):
            walk.dev_pipeline.require_companion_stopped()
            fd = os.open(runner.root / 'execution-private.log', os.O_CREAT | os.O_WRONLY | os.O_APPEND, 0o600)
            with os.fdopen(fd, 'a') as stream, redirect_stdout(stream):
                fixture = runner.plan['fixtures'][0]
                if args.action == 'run':
                    require(args.backup_confirmed and args.no_unrecovered_transfers,
                            'Raw backup and no unrecovered transfers must be acknowledged')
                    walk.save_json(runner.root / 'authorization.json', {'planSHA256': walk.digest(args.plan.read_bytes()),
                                   'backupConfirmed': True, 'noUnrecoveredTransfers': True})
                    runner.execute_fixture(fixture)
                else:
                    auth = walk.load_json(runner.root / 'authorization.json')
                    require(auth.get('planSHA256') == walk.digest(args.plan.read_bytes()) and auth.get('backupConfirmed') is True,
                            'Original authorization changed')
                    folder = runner.root / fixture['label']
                    report = walk.load_json(folder / 'report.json')
                    require(report.get('stage') == 'walk-submission-prepared' and report.get('run') == fixture['run'],
                            'Observe only a pending one-shot Library trial; cleanup is never replayed')
                    try:
                        runner.finish(fixture, folder, report)
                    except BaseException as error:
                        report.update(passed=False, stageAtFailure=report['stage'], errorType=type(error).__name__,
                                      error=str(error) if isinstance(error, walk.WalkError) else 'Observation stopped',
                                      cleanupMayBePartial=report.get('cleanupStarted', False), localRecoveryEvidencePreserved=True)
                        runner.persist(folder, report)
                        raise
        print(f'Private Library evidence: {runner.root}')
        return 0
    except (walk.WalkError, OSError, ValueError, KeyError, zipfile.BadZipFile, ET.ParseError, urllib.error.URLError) as error:
        print(str(error) if isinstance(error, walk.WalkError) else 'Library trial stopped; inspect private evidence.', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
