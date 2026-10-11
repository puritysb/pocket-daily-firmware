#!/usr/bin/env python3
"""Local app/reader development loop. See docs/developer-pipeline.md.

Same Wi-Fi only; no AP switching. Explicit --flash authorizes developer install.
No mutation is automatically replayed after an unknown outcome.
"""
import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import fcntl
import hashlib
import ipaddress
import json
import os
import plistlib
from pathlib import Path
import re
import secrets
import shutil
import signal
import subprocess
import sys
import time

import dev_ble_cycle as cycle
import pocket_put

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CONFIG = ROOT / 'build/dev-pipeline.json'
MARKER = 'POCKET_HARDWARE_RESULT '


def save(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')
    temporary.replace(path)


@contextmanager
def exclusive(path):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('a') as handle:
        try:
            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise RuntimeError('another developer pipeline owns this reader checkout') from error
        try:
            yield
        finally:
            fcntl.flock(handle, fcntl.LOCK_UN)


def source_state(root):
    def git(*args):
        return subprocess.check_output(['git', *args], cwd=root)
    digest = hashlib.sha256(git('diff', 'HEAD', '--binary'))
    untracked = git('ls-files', '--others', '--exclude-standard', '-z').split(b'\0')
    for name in sorted(filter(None, untracked)):
        digest.update(name)
        path = root / os.fsdecode(name)
        if path.is_file():
            digest.update(path.read_bytes())
    return {'head': git('rev-parse', 'HEAD').decode().strip(),
            'dirty': bool(git('status', '--porcelain')),
            'workingChangesSHA256': digest.hexdigest()}


INTERFACE_CASES = {'identity-rejection', 'storage-usage', 'preferences-roundtrip', 'profile-roundtrip-conflict',
                   'batch-article-download', 'downloaded-book-reading', 'pause-resume', 'pause-discard',
                   'folder-copy', 'inventory-preservation'}


def hardware_evidence(log, run, version, standby, platform, interface=False, wifi_setup=False, screens=False):
    records = []
    for line in log.splitlines():
        if MARKER in line:
            records.append(json.loads(line.split(MARKER, 1)[1]))
    if len(records) != 1:
        raise RuntimeError('expected exactly one hardware result; a skipped test is not proof')
    record = records[0]
    if (record.get('run') != str(run) or record.get('version') != version
            or record.get('passed') is not True or record.get('initialConnection') is not True
            or record.get('platform') != platform):
        raise RuntimeError('hardware result failed or does not match this run/artifact/platform')
    if wifi_setup and record.get("wifiSetupRollback") != "passed":
        raise RuntimeError("missing verified Wi-Fi setup rollback")
    if screens:
        visual = record.get('screenValidation', {})
        captures = visual.get('captures', [])
        expected_names = ['01-home-reading', '02-home-word', '03-home-reading-restored',
                          '04-home-weather-bottom', '05-home-weather-top', '06-brief-weather',
                          '07-brief-today', '08-brief-weather-restored', '09-card-image-a',
                          '10-card-image-b', '11-card-image-restored', '12-reader-body', '13-actual-sleep']
        if (visual.get('passed') is not True or visual.get('restored') is not True
                or visual.get('staleCaptureRejected') is not True
                or [c.get('name') for c in captures] != expected_names):
            raise RuntimeError('missing or failed physical screen validation')
        sleep = visual.get('sleepCycle', {})
        if (visual.get('cardsRestored') is not True or visual.get('readingPreserved') is not True
                or sleep.get('run') != run + 13
                or sleep.get('state') != 3 or sleep.get('returnMode') != 1
                or sleep.get('timerArmed') is not True or sleep.get('timerWake') is not True):
            raise RuntimeError('missing card restoration or actual sleep/wake evidence')
        differences = visual.get('pixelDifferences', {})
        if differences.get('cardImage', 0) <= 1000 or differences.get('cardRestored') != 0:
            raise RuntimeError('missing card image pixel validation')
        for index, capture in enumerate(captures):
            if (capture.get('run') != run + index + 1 or capture.get('width') != 528
                    or capture.get('height') != 792 or len(capture.get('sha256', '')) != 64):
                raise RuntimeError('stale or invalid reader capture evidence')
    if interface:
        cases = record.get('interfaceCases', [])
        if len(cases) != len(INTERFACE_CASES) or {c.get('id') for c in cases} != INTERFACE_CASES:
            raise RuntimeError('missing or duplicated interface cases')
        for case in cases:
            expected = 'not-applicable' if platform == 'iOS-device' and case['id'] == 'folder-copy' else 'passed'
            if case.get('state') != expected:
                raise RuntimeError('interface case did not pass: ' + case['id'])
        if record.get('transferFeedbackRecovery') != 'passed':
            raise RuntimeError('interface suite requires firmware feedback recovery')
    if standby:
        trial = record.get('cycle', {})
        if trial.get('run') != run:
            raise RuntimeError('stale firmware cycle evidence')
        cycle.check_result(trial, standby=True)
        if record.get('standbyWaitSeconds', 0) < 90 or trial.get('standbyMs', 0) < 60000:
            raise RuntimeError('standby trial did not span the first minute eligibility check')
    return record


def collect_screen_artifacts(destination, captures):
    manifest = json.loads((destination / 'manifest.json').read_text())
    attachments = [item for test in manifest for item in test['attachments']]
    for capture in captures:
        for suffix in ['bmp', 'png']:
            name = capture['name'] + '.' + suffix
            # xcresulttool removes the attachment's extension before appending
            # its activity counter/UUID, then adds the extension again.
            matches = [item for item in attachments
                       if item['suggestedHumanReadableName'].startswith(capture['name'] + '_')
                       and item['exportedFileName'].endswith('.' + suffix)]
            if len(matches) != 1:
                raise RuntimeError('Missing or ambiguous screen attachment: ' + name)
            exported = matches[0]['exportedFileName']
            if Path(exported).name != exported:
                raise RuntimeError('Invalid exported attachment path')
            payload = (destination / exported).read_bytes()
            if suffix == 'bmp' and hashlib.sha256(payload).hexdigest() != capture['sha256']:
                raise RuntimeError('Saved capture does not match hardware evidence')
            (destination / name).write_bytes(payload)


class Pipeline:
    def __init__(self, output):
        self.output = output
        output.mkdir(parents=True, exist_ok=False, mode=0o700)
        self.report = {'started': datetime.now(timezone.utc).isoformat(), 'passed': False, 'stages': [],
                       'iosPhysical': 'not run', 'currentMeasurement': 'not measured'}
        self.flush()

    def flush(self):
        save(self.output / 'report.json', self.report)

    def stage(self, name, operation):
        entry = {'name': name, 'state': 'running'}
        self.report['stages'].append(entry)
        self.flush()
        print(name, flush=True)
        start = time.monotonic()
        try:
            result = operation()
            entry['state'] = 'passed'
            return result
        except BaseException as error:
            entry.update(state='failed', error=str(error) or type(error).__name__)
            raise
        finally:
            entry['seconds'] = round(time.monotonic() - start, 2)
            self.flush()

    def command(self, name, command, cwd=ROOT, env=None, timeout=1800):
        log = self.output / (name + '.log')
        def execute():
            with log.open('w') as stream:
                process = subprocess.Popen(command, cwd=cwd, env=env, stdout=stream,
                                           stderr=subprocess.STDOUT, start_new_session=True)
                try:
                    code = process.wait(timeout=timeout)
                except BaseException:
                    os.killpg(process.pid, signal.SIGTERM)
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL)
                        process.wait()
                    raise
                if code:
                    raise RuntimeError(f'{name}: exit {code}; see {log}. Mutations were not retried.')
            return log
        return self.stage(name, execute)


def private_host(value):
    address = ipaddress.ip_address(value)
    if address.version != 4 or not address.is_private or address.is_loopback or address.is_unspecified:
        raise ValueError('requires a private LAN IPv4 reader address')
    return str(address)


def require_companion_stopped():
    # Xcode can launch a second copy with the same bundle ID. In particular,
    # its normal heartbeat must not probe while port-82 upload owns the reader.
    processes = subprocess.check_output(['ps', '-axo', 'pid=,command='], text=True)
    for line in processes.splitlines():
        match = re.match(r"\s*(\d+)\s+(.+?\.app)/Contents/MacOS/Pocket(?:\s|$)", line)
        if not match:
            continue
        try:
            with (Path(match[2]) / 'Contents/Info.plist').open('rb') as stream:
                bundle = plistlib.load(stream)
        except (OSError, ValueError):
            raise RuntimeError('Cannot identify an existing Pocket app; stop it before the hardware pipeline')
        if bundle.get('CFBundleIdentifier') == 'bound.serendipity.pocket.daily':
            raise RuntimeError('Pocket Daily is already running. Quit it before this pipeline so only '
                               'the XCTest-hosted app owns the reader. No install/cycle was started.')


def reader_status(host, identity, version=None):
    status = cycle.get_json(host, '/api/status')
    cycle.verify_identity(status, identity, version)
    if status.get('mode') != 'STA' or '-dev-' not in status.get('version', ''):
        raise RuntimeError('requires an enrolled developer reader in Same Wi-Fi mode')
    return status


def run(args, config, job):
    job.stage('exclusive-app-preflight', require_companion_stopped)
    host, identity = private_host(config['host']), config['deviceID']
    app = args.app.resolve()
    job.report['sources'] = {'firmware': source_state(ROOT), 'app': source_state(app),
                             'sdk': source_state(ROOT / 'freeink-sdk')}
    status = job.stage('reader-preflight', lambda: reader_status(host, identity))
    version = status['version']
    if args.standby and status.get('device') != 'X3':
        raise RuntimeError('experimental standby hardware scenario is X3 only')
    if args.checks or args.build_firmware:
        commands = [
            ('host-configure', ['cmake', '-S', 'test', '-B', 'build/host-tests', '-DCMAKE_BUILD_TYPE=Release']),
            ('host-build', ['cmake', '--build', 'build/host-tests', '-j8']),
            ('host-tests', ['ctest', '--test-dir', 'build/host-tests', '--output-on-failure', '-j8']),
            ('pipeline-tests', [sys.executable, '-m', 'unittest', 'discover', '-s', 'scripts', '-p', 'test_dev_*.py']),
            ('upload-tests', [sys.executable, '-m', 'unittest', 'discover', '-s', 'scripts', '-p', 'test_pocket_put.py']),
            ('route-tests', [sys.executable, '-m', 'unittest', 'discover', '-s', 'scripts', '-p', 'test_sync_routes.py']),
            ('cppcheck', [sys.executable, 'scripts/check_firmware.py', '--cppcheck', args.cppcheck]),
            ('default-build', ['./scripts/pio.sh', 'run', '-e', 'default']),
        ]
        for name, command in commands:
            job.command(name, command)
    if args.build_firmware == 'ble_standby':
        job.command('standby-build', ['./scripts/pio_ble_standby.sh'])
    image = ROOT / 'firmware/update.bin' if args.build_firmware else args.firmware
    if image:
        payload = image.read_bytes()
        version = pocket_put.developer_version(payload)
        frozen = job.output / 'candidate.bin'
        frozen.write_bytes(payload)
        job.report['artifact'] = {'version': version, 'bytes': len(payload),
                                  'sha256': hashlib.sha256(payload).hexdigest()}
        if args.build_firmware:
            shutil.copyfile(ROOT / 'firmware/LATEST_BUILD.txt', job.output / 'LATEST_BUILD.txt')
    if args.standby and 'ble-standby' not in version:
        raise RuntimeError('standby scenario requires the experimental ble_standby artifact')
    job.report['version'] = version
    job.command('app-project', ['xcodegen', 'generate'], cwd=app)
    derived = app / '.build/dev-pipeline-mac'
    base = ['xcodebuild', '-project', 'Pocket.xcodeproj', '-scheme', 'PocketMac', '-configuration', 'Debug',
            '-destination', 'platform=macOS,arch=arm64', '-derivedDataPath', str(derived), 'CODE_SIGNING_ALLOWED=YES']
    job.command('mac-build', base + ['build-for-testing'], cwd=app)
    app_binary = derived / 'Build/Products/Debug/Pocket.app/Contents/MacOS/Pocket'
    job.report['macApp'] = {'path': str(app_binary), 'sha256': hashlib.sha256(app_binary.read_bytes()).hexdigest()}
    if args.ios_simulator:
        job.command('ios-simulator', ['xcodebuild', 'test', '-project', 'Pocket.xcodeproj', '-scheme', 'Pocket',
            '-configuration', 'Debug', '-destination', f'platform=iOS Simulator,id={args.ios_simulator}',
            '-derivedDataPath', str(app / '.build/dev-pipeline-ios'), 'CODE_SIGNING_ALLOWED=YES',
            '-only-testing:PocketTests/ReaderWakeConnectorTests',
            '-only-testing:PocketTests/ReaderBluetoothLinkTests',
            '-only-testing:PocketTests/ReadingSyncBLEProtocolTests',
            '-only-testing:PocketTests/NearbySyncProtocolTests',
            '-only-testing:PocketTests/ReaderHardwareTests'], cwd=app)
        job.report['iosSimulator'] = 'passed; physical hardware scenario skipped'
    job.report['testedSources'] = {'firmware': source_state(ROOT), 'app': source_state(app),
                             'sdk': source_state(ROOT / 'freeink-sdk')}
    platforms = [('macOS', base, 'PocketMacTests')]
    if args.ios_device:
        ios = ['xcodebuild', '-project', 'Pocket.xcodeproj', '-scheme', 'Pocket', '-configuration', 'Debug',
               '-destination', f'platform=iOS,id={args.ios_device}', '-derivedDataPath', str(app / '.build/dev-pipeline-device'),
               'CODE_SIGNING_ALLOWED=YES']
        job.command('ios-device-build', ios + ['build-for-testing'], cwd=app)
        platforms.append(('iOS-device', ios, 'PocketTests'))
    job.stage('exclusive-app-before-hardware', require_companion_stopped)
    if image:
        current = job.stage('reader-before-install', lambda: reader_status(host, identity))
        if args.flash and current['version'] != version:
            job.command('upload-install-reboot', [sys.executable, str(ROOT / 'scripts/pocket_put.py'),
                str(frozen), '--host', host, '--target', 'update.bin', '--resume', '--dev-flash'], timeout=600)
        elif args.flash:
            job.report['installation'] = 'exact version already running; flash skipped'
        job.stage('installed-version', lambda: reader_status(host, identity, version))
    job.report['hardware'] = []
    for platform, command, target in platforms:
        for index in range(args.cycles):
            run_id = secrets.randbelow(0xffffffff - 16) + 1  # reserve capture sequence IDs
            env = {key: value for key, value in os.environ.items() if not key.startswith('TEST_RUNNER_POCKET_HARDWARE_')}
            for key, value in {'ENABLED': '1', 'HOST': host, 'DEVICE_ID': identity, 'VERSION': version,
                               'RUN': str(run_id), 'STANDBY': '1' if args.standby else '0',
                               'SCREENS': '1' if getattr(args, 'screen_suite', False) and platform == 'macOS' else '0',
                               'WIFI_SETUP': '1' if getattr(args, 'wifi_setup', False) else '0',
                               'INTERFACE': '1' if getattr(args, 'interface_suite', False) else '0'}.items():
                env['TEST_RUNNER_POCKET_HARDWARE_' + key] = value
            label = f'{platform}-hardware-{index + 1}'
            log = job.command(label, command + ['test-without-building', '-parallel-testing-enabled', 'NO',
                '-only-testing:' + target + '/ReaderHardwareTests/testDeveloperConnectionCycle',
                '-resultBundlePath', str(job.output / (label + '.xcresult'))], cwd=app, env=env, timeout=1200 if getattr(args, 'interface_suite', False) else 480)
            evidence = job.stage(label + '-evidence', lambda: hardware_evidence(
                log.read_text(), run_id, version, args.standby, platform, getattr(args, 'interface_suite', False), getattr(args, 'wifi_setup', False), getattr(args, 'screen_suite', False) and platform == 'macOS'))
            if getattr(args, 'screen_suite', False) and platform == 'macOS':
                destination = job.output / (label + '-screens')
                job.command(label + '-export-screens', ['xcrun', 'xcresulttool', 'export', 'attachments',
                    '--path', str(job.output / (label + '.xcresult')), '--output-path', str(destination)])
                collect_screen_artifacts(destination, evidence['screenValidation']['captures'])
                job.command(label + '-screen-semantics', ['xcrun', 'swift',
                    str(app / 'scripts/verify_reader_screens.swift'), str(destination)], cwd=app)
                semantic = json.loads((destination / 'semantic-validation.json').read_text())
                if semantic.get('passed') is not True:
                    raise RuntimeError('Reader image semantics did not pass')
                evidence['screenValidation']['semanticValidation'] = semantic
                evidence['screenValidation']['exportedDirectory'] = str(destination)
            job.report['hardware'].append(evidence)
            if platform == 'iOS-device':
                job.report['iosPhysical'] = 'passed'
            job.flush()
    job.report['passed'] = True
    job.flush()


def main():
    os.umask(0o077)
    for key in list(os.environ):
        if key.startswith(('TEST_RUNNER_POCKET_HARDWARE_', 'POCKET_HARDWARE_')):
            del os.environ[key]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['configure', 'run'])
    parser.add_argument('--config', type=Path, default=DEFAULT_CONFIG)
    parser.add_argument('--host', help='configure only: enroll an identified developer reader')
    parser.add_argument('--app', type=Path, default=ROOT.parent / 'pocket-daily')
    parser.add_argument('--firmware', type=Path, help='snapshot and verify this developer artifact')
    parser.add_argument('--build-firmware', choices=['default', 'ble_standby'])
    parser.add_argument('--flash', action='store_true', help='explicitly install the chosen developer artifact')
    parser.add_argument('--wifi-setup', action='store_true', help='BLE Wi-Fi setup with nonexistent network and verified rollback')
    parser.add_argument('--screen-suite', action='store_true', help='physical X3 Home/Brief/cards/EPUB captures and actual deep-sleep validation')
    parser.add_argument('--interface-suite', action='store_true', help='full app interface scenarios with owned fixtures and restoration')
    parser.add_argument('--checks', action='store_true', help='full firmware host/analysis/default-build gates')
    parser.add_argument('--cppcheck', default='build/cppcheck-source/cppcheck')
    parser.add_argument('--standby', action='store_true', help='actual app BLE wake scenario (experimental X3)')
    parser.add_argument('--cycles', type=int, default=1)
    parser.add_argument('--ios-simulator', help='simulator UDID for protocol regression/build validation')
    parser.add_argument('--ios-device', help='physical iPhone UDID; already paired/permissioned Debug app required')
    args = parser.parse_args()
    if args.action == 'run' and args.host:
        parser.error('--host is enrollment only; use configure to change the expected reader')
    if args.standby and args.build_firmware == 'default':
        parser.error('--standby requires --build-firmware ble_standby')
    if not 1 <= args.cycles <= 20:
        parser.error('cycles must be 1..20')
    if args.firmware and args.build_firmware:
        parser.error('choose --firmware or --build-firmware')
    if args.flash and not (args.firmware or args.build_firmware):
        parser.error('--flash requires a developer artifact')
    with exclusive(ROOT / 'build/dev-pipeline.lock'):
        if args.action == 'configure':
            if not args.host:
                parser.error('configure requires --host')
            host = private_host(args.host)
            status = cycle.get_json(host, '/api/status')
            if not status.get('deviceID') or status.get('mode') != 'STA' or '-dev-' not in status.get('version', ''):
                raise RuntimeError('only identified developer readers in Same Wi-Fi can be enrolled')
            save(args.config, {'host': host, 'deviceID': status['deviceID']})
            print(f'Local reader enrolled: {args.config}')
            return
        config = json.loads(args.config.read_text())
        stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S') + '-' + secrets.token_hex(3)
        job = Pipeline(ROOT / 'build/dev-pipeline-runs' / stamp)
        print(f'Report: {job.output / "report.json"}', flush=True)
        try:
            run(args, config, job)
        except BaseException as error:
            job.report['error'] = str(error) or type(error).__name__
            # Read only after the failed subprocess has drained; never probe
            # alongside a live upload or replay an unknown mutation.
            try:
                if not any(s['name'] == 'reader-preflight' and s['state'] == 'passed'
                           for s in job.report['stages']):
                    raise RuntimeError('reader preflight did not complete')
                require_companion_stopped()
                observed = cycle.get_json(config['host'], '/api/status', timeout=2)
                cycle.verify_identity(observed, config['deviceID'])
                job.report['readerOnFailure'] = observed
                job.report['cycleOnFailure'] = cycle.get_json(
                    config['host'], '/api/pocket/v1/dev/ble-cycle', timeout=2)
            except (Exception, KeyboardInterrupt):
                job.report['failureProbe'] = 'reader evidence unavailable'
            job.flush()
            raise
        print(f'PASS: {job.output / "report.json"}')


if __name__ == '__main__':
    try:
        main()
    except (Exception, KeyboardInterrupt) as error:
        print(f'FAILED: {error}', file=sys.stderr)
        sys.exit(1)
