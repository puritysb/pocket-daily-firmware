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
import math
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
# Hash the checked-in, image-reduced fixture, not the original EPUB in the XPointer golden metadata.
REAL_EPUB_SHA256 = 'd09a9d94bd1076edd07a9204989ed2917bef73ad28cbd855d758641fb2e5eeb0'
REAL_EPUB_BYTES = 226309


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


# Seed, the unarmed control, then per round: app offer, post-acceptance list and reader offer.
BLE_CONTINUITY_LEGS = [('seed', True, 0), ('control-unarmed', False, 0),
                       ('app-to-x3-0', True, 1), ('after-accept-0', True, 0), ('x3-to-app-0', True, 0),
                       ('app-to-x3-1', True, 1), ('after-accept-1', True, 0), ('x3-to-app-1', True, 0)]


def check_ble_continuity(reading, run):
    """Every position exchange went through the bonded BLE standby window.

    Each leg needs its own run-bound reader record: the window opened, the
    reader stored exactly the offers the app sent (none without an armed link)
    and the production app wake returned it to Same Wi-Fi.
    """
    cycles = reading.get('bluetoothCycles', [])
    if (reading.get('transport') != 'bluetooth' or len(cycles) != len(BLE_CONTINUITY_LEGS)
            or any(c.get('transport') != 'bluetooth' for c in reading.get('cases', []))):
        raise RuntimeError('missing Bluetooth-only reading exchange evidence')
    for index, (cycle, (leg, armed, offers)) in enumerate(zip(cycles, BLE_CONTINUITY_LEGS)):
        reader = cycle.get('reader', {})
        expected_run = (run + 20 + 100 + index + 1) % (2 ** 32)
        if (cycle.get('leg') != leg or cycle.get('armed') is not armed or cycle.get('run') != expected_run
                or reader.get('run') != expected_run or reader.get('state') != 3
                or reader.get('close') != 'app-wifi' or reader.get('fromWifi') is not True
                or reader.get('gate') != 'open' or reader.get('offers') != offers
                or (reader.get('lists', 0) < 1 if armed else reader.get('lists') != 0)
                or (armed and cycle.get('sent') != offers)
                or reader.get('minFree', 0) <= 50 * 1024 or reader.get('minBlock', 0) < 8 * 1024):
            raise RuntimeError('Bluetooth reading exchange evidence failed for ' + leg)


STARTER_COLLECTIONS = ['everyday', 'ko-everyday', 'reading', 'ko-reading', 'focus', 'learning',
                       'writing', 'ko-english-travel', 'ko-english-work', 'ko-japanese-basics',
                       'ko-japanese-travel', 'ko-review']
STARTER_PRESETS = [('Home', name) for name in ('quiet', 'daily', 'study', 'vocabulary')] + [
    ('Sleep', name) for name in ('quiet', 'daily', 'study')]


def check_x4_starters(record, layout_only=False):
    detail = record.get('x4StartersValidation', {})
    cards, collections, presets, captures = [detail.get(key, []) for key in ('cards', 'collections', 'presets', 'captures')]
    names = [c.get('name') for c in captures]
    expected = []
    for collection in collections:
        ids = collection.get('cards', [])
        if len(ids) != 3 or len(set(ids)) != 3:
            raise RuntimeError('missing three-card collection evidence')
        expected += ['starter-card-' + card for card in ids[1:] + ids[:1]]
        expected += ['starter-retained-' + collection['id']]
    expected += [f'preset-{scope.lower()}-{name}{suffix}' for scope, name in STARTER_PRESETS for suffix in ('', '-retained')]
    health = detail.get('health', [])
    health_names = [c.get('card') for c in cards] + [f'preset-{scope.lower()}-{name}' for scope, name in STARTER_PRESETS]
    if (record.get('device') != 'X4' or detail.get('passed') is not True or detail.get('restored') is not True
            or detail.get('layoutPreviewParity') is not True or detail.get('layoutOnly') is not layout_only
            or len(cards) != (0 if layout_only else 36) or len({c.get('card') for c in cards}) != (0 if layout_only else 36)
            or [c.get('id') for c in collections] != ([] if layout_only else STARTER_COLLECTIONS)
            or [(p.get('scope'), p.get('id')) for p in presets] != STARTER_PRESETS
            or any(c.get('pixelDifference') != 0 or c.get('readBack') is not True
                   or not isinstance(c.get('applySeconds'), (int, float)) or isinstance(c.get('applySeconds'), bool)
                   or not math.isfinite(c['applySeconds']) or c['applySeconds'] <= 0 for c in cards)
            or any(c.get('rebootPersisted') is not True or c.get('retainedPixelDifference') != 0 for c in collections)
            or any([c.get('card') for c in cards if c.get('collection') == group['id']] != group['cards'][1:] + group['cards'][:1]
                   for group in collections)
            or any(p.get('profileReadBack') is not True or p.get('unrelatedSettingsPreserved') is not True
                   or p.get('rebootPersisted') is not True or p.get('cardsPreserved') is not True
                   or p.get('retainedBodyPixelDifference') != 0
                   or p.get('previewUsesReaderData') is not True
                   or p.get('previewPixelDifference') != 0 or p.get('retainedPreviewPixelDifference') != 0
                   or p.get('changeFromPrevious', 0) <= 500 and p.get('id') != 'quiet' for p in presets)
            or names != expected or len(set(names)) != (14 if layout_only else 62)
            or [item.get('case') for item in health] != health_names
            or any(type(item.get('freeHeap')) is not int or item['freeHeap'] < 16384
                   or type(item.get('uptime')) is not int or item['uptime'] < 0
                   or type(item.get('crashReportAvailable')) is not bool for item in health)
            or any(c.get('width') != 480 or c.get('height') != 800
                   or not re.fullmatch('[0-9a-f]{64}', c.get('sha256', '')) for c in captures)):
        raise RuntimeError('missing X4 starter cards/presets/reboot acceptance evidence')


def check_x4_sleep(record):
    detail = record.get('x4SleepValidation', {})
    cases, captures = detail.get('cases', []), detail.get('captures', [])
    run = int(record.get('run', 0))
    if (record.get('device') != 'X4' or detail.get('passed') is not True or detail.get('restored') is not True
            or len(cases) != 7 or len(captures) != 49):
        raise RuntimeError('missing full actual X4 Home/sleep/wake evidence')
    homes = ['quiet', 'daily', 'study', 'vocabulary', 'quiet', 'daily', 'study']
    sleeps = ['quiet', 'daily', 'study', 'quiet', 'quiet', 'daily', 'study']
    for i, case in enumerate(cases):
        cycle = case.get('cycle', {})
        if (case.get('home') != homes[i] or case.get('sleep') != sleeps[i]
                or case.get('sleepWakeIndicator') is not (i < 4) or case.get('index') != i or case.get('run') != run + i + 1 or case.get('pixelDifferences') != [0, 0, 0]
                or case.get('settingsRetained') is not True or cycle.get('run') != run + i + 1
                or cycle.get('timerArmed') is not True or cycle.get('timerWake') is not True
                or cycle.get('usbPower') is not True or cycle.get('state') != 3 or cycle.get('returnMode') != 3
                or cycle.get('returnedHomeRun') != run + i + 1 or cycle.get('wakeCaptureRun') != run + i + 1 or cycle.get('wakeSkipped') is not False
                or case.get('wakePixels', 0) <= 100):
            raise RuntimeError('invalid actual X4 deep-sleep/cue/preview retention evidence')
    expected = [f'actual-{i}-{name}' for i in range(7) for name in ('home', 'home-preview', 'sleep', 'sleep-preview', 'returned-home', 'returned-home-preview', 'wake')]
    if ([c.get('name') for c in captures] != expected
            or any(c.get('run') != run + i // 7 + 1 or c.get('width') != 480 or c.get('height') != 800
                   or len(c.get('sha256', '')) != 64 for i, c in enumerate(captures))):
        raise RuntimeError('stale or missing actual X4 frame attachments')


def check_x4_settings(record, typography_only=False):
    detail = record.get('x4SettingsValidation', {})
    cases = detail.get('cases', [])
    captures = detail.get('captures', [])
    typography = detail.get('typography', [])
    typography_fields = ('fontSize', 'lineSpacing', 'screenMargin')
    orientations = [0] if typography_only else [0, 1, 2, 3]
    if (record.get('device') != 'X4' or detail.get('passed') is not True or detail.get('restored') is not True
            or [c.get('orientation') for c in cases] != orientations
            or any(c.get('preferencesReadBack') is not True or c.get('profileReadBack') is not True
                   or c.get('rebootPersisted') is not True or c.get('homeRetainedDifference') != 0 or c.get('cardPixelDifference') != 0 or c.get('readingFrameChanged') is not True or c.get('cardPersisted') is not True for c in cases)
            or [c.get('name') for c in captures] != ([f'x4-{surface}-{i}' for i in orientations for surface in ('home', 'home-retained', 'brief', 'card', 'reading')]
                                                   + [f'x4-typography-{field}' for field in typography_fields])
            or [c.get('field') for c in typography] != list(typography_fields)
            or any(c.get('rebootPersisted') is not True or c.get('bodyPixelDifference', 0) <= 500 for c in typography)
            or any(c.get('width') != 480 or c.get('height') != 800 or len(c.get('sha256', '')) != 64 for c in captures)):
        raise RuntimeError('missing X4 settings/preview/reboot acceptance evidence')


def check_x4_continuity(record):
    reading = record.get('continuityValidation', {})
    epub = reading.get('epub', {})
    if (record.get('device') != 'X4' or record.get('model') != 'X4'
            or type(record.get('bluetoothCalls')) is not int or record['bluetoothCalls'] != 0
            or record.get('identityVerified') is not True
            or record.get('sharedReaderExchangeIsolatedFromStartup') is not True
            or record.get('glanceSourcesUnconfigured') is not True
            or reading.get('model') != 'X4' or reading.get('transport') != 'http'
            or reading.get('bluetoothCycles') != []
            or any(c.get('transport') != 'http' for c in reading.get('cases', []))
            or any(c.get('width') != 480 or c.get('height') != 800
                   for c in reading.get('captures', []))
            or epub.get('kind') != 'generated' or not isinstance(epub.get('sha256'), str)
            or re.fullmatch(r'[0-9a-f]{64}', epub['sha256']) is None
            or type(epub.get('bytes')) is not int or not 0 < epub['bytes'] <= 4 * 1024 * 1024
            or epub.get('roundTripBytesMatch') is not True or epub.get('libraryReimportMatches') is not True):
        raise RuntimeError('missing isolated X4 HTTP continuity evidence')


def check_x4_continuity_bitmaps(destination, captures):
    for capture in captures:
        data = (destination / (capture['name'] + '.bmp')).read_bytes()
        if (len(data) < 54 or data[:2] != b'BM'
                or int.from_bytes(data[18:22], 'little', signed=True) != 480
                or int.from_bytes(data[22:26], 'little', signed=True) != 800):
            raise RuntimeError('X4 continuity capture does not have native 480x800 dimensions')


def hardware_evidence(log, run, version, standby, platform, interface=False, wifi_setup=False, screens=False, standby_seconds=90, battery_only=False, delayed_advertising=False, wake_control=False, network_faults=False, continuity=False, real_epub=False, ble_continuity=False):
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
    if continuity:
        reading = record.get('continuityValidation', {})
        cases = reading.get('cases', [])
        captures = reading.get('captures', [])
        names = ['continuity-seed', 'continuity-app-to-x3-0', 'continuity-x3-to-app-0',
                 'continuity-app-to-x3-1', 'continuity-x3-to-app-1']
        if (reading.get('passed') is not True or reading.get('restored') is not True
                or len(cases) != 2 or [c.get('name') for c in captures] != names
                or any(c.get('run') != (run + 20) % (2 ** 32) or len(c.get('sha256', '')) != 64 for c in captures)
                or [c.get('round') for c in cases] != [0, 1]
                or any(c.get('passed') is not True or c.get('direction') != 'bidirectional'
                       or c.get('noMovementBeforeAcceptance') is not True
                       or c.get('acceptedCharacterVisible') is not True
                       or c.get('navigatedFromAnotherChapter') is not True for c in cases)
                or [c.get('backwardOffer') for c in cases] != [False, True]):
            raise RuntimeError('missing bidirectional reading acceptance evidence')
        if ble_continuity:
            check_ble_continuity(reading, run)
    if real_epub:
        epub = record.get('continuityValidation', {}).get('epub', {})
        if (not continuity or epub.get('kind') != 'published-book'
                or epub.get('sha256') != REAL_EPUB_SHA256
                or epub.get('bytes') != REAL_EPUB_BYTES or epub.get('roundTripBytesMatch') is not True
                or epub.get('libraryReimportMatches') is not True):
            raise RuntimeError('missing unchanged published EPUB round-trip evidence')
    if wifi_setup and record.get("wifiSetupRollback") != "passed":
        raise RuntimeError("missing verified Wi-Fi setup rollback")
    if screens:
        visual = record.get('screenValidation', {})
        captures = visual.get('captures', [])
        expected_names = ['01-home-reading', '02-home-word', '03-home-reading-restored',
                          '04-home-weather-bottom', '05-home-weather-top', '06-brief-weather',
                          '07-brief-today', '08-brief-weather-restored', '09-card-image-a',
                          '10-card-image-b', '11-card-image-restored', '12-reader-body', '13-actual-sleep', '14-wake-transition']
        if (visual.get('passed') is not True or visual.get('restored') is not True
                or visual.get('staleCaptureRejected') is not True
                or [c.get('name') for c in captures] != expected_names):
            raise RuntimeError('missing or failed physical screen validation')
        sleep = visual.get('sleepCycle', {})
        if (visual.get('cardsRestored') is not True or visual.get('readingPreserved') is not True
                or visual.get('preferencesRestored') is not True
                or sleep.get('run') != run + 13
                or sleep.get('state') != 3 or sleep.get('returnMode') != 1
                or sleep.get('timerArmed') is not True or sleep.get('timerWake') is not True
                or sleep.get('wakeCaptureRun') != run + 13 or sleep.get('wakeSkipped') is not False
                or not 0 < sleep.get('wakeRefreshMs', 0) <= 1500):
            raise RuntimeError('missing card restoration or actual sleep/wake evidence')
        differences = visual.get('pixelDifferences', {})
        if (differences.get('sleepContentRetained') != 0 or differences.get('wakeBodyRetained') != 0
                or differences.get('wakeTransition', 0) <= 100):
            raise RuntimeError('actual sleep body differs from selected Brief')
        if differences.get('cardImage', 0) <= 1000 or differences.get('cardRestored') != 0:
            raise RuntimeError('missing card image pixel validation')
        for index, capture in enumerate(captures):
            if (capture.get('run') != run + min(index + 1, 13) or capture.get('width') != 528
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
        if not delayed_advertising and trial.get("advertisingDelayMs", 0) != 0:
            raise RuntimeError("advertising fault leaked into ordinary standby")
        if standby_seconds > 90:
            if trial.get('standbyLimitMs') != (standby_seconds + 90) * 1000:
                raise RuntimeError('missing long-standby recovery deadline')
            if record.get('idleAppTrafficSuspended') is not True:
                raise RuntimeError('long standby requires suspended app traffic')
        if record.get('standbyWaitSeconds', 0) != standby_seconds or trial.get('standbyMs', 0) < (standby_seconds - 30) * 1000:
            raise RuntimeError('standby trial did not span the first minute eligibility check')
    if delayed_advertising:
        trial = record.get('cycle', {})
        delay_ms = (standby_seconds + 16) * 1000
        if (not standby or trial.get('advertisingDelayMs') != delay_ms
                or trial.get('advertisingStartedMs', 0) < delay_ms):
            raise RuntimeError('missing reader advertising delay')
    if wake_control:
        if (not delayed_advertising or record.get('wakeControl') is not True
                or [item.get('event') for item in record.get('wakeObservations', [])]
                    != ['connecting', 'connected', 'authenticated', 'commandSent', 'accepted']):
            raise RuntimeError('invalid no-rediscovery control evidence')
    if delayed_advertising and not wake_control:
        trial = record.get('cycle', {})
        delay_ms = (standby_seconds + 16) * 1000
        observations = record.get('wakeObservations', [])
        names = [item.get('event') for item in observations]
        times = {item.get('event'): item.get('seconds') for item in observations}
        scan, connected = times.get('discoveryStarted'), times.get('connected')
        ordered = [item.get('seconds') for item in observations]
        if (not standby or trial.get('advertisingDelayMs') != delay_ms
                or trial.get('advertisingStartedMs', 0) < delay_ms
                or record.get('wakeControl') is True
                or names != ['connecting', 'connectionReset', 'pendingConnectionCancelled', 'discoveryStarted',
                             'knownAdvertisement', 'reconnectRequested', 'connected', 'authenticated',
                             'commandSent', 'accepted']
                or not isinstance(scan, (int, float)) or not isinstance(connected, (int, float))
                or not 7.5 <= scan < connected < 20
                or not all(isinstance(value, (int, float)) for value in ordered)
                or not 0 <= ordered[0] <= ordered[-1] < 20
                or not all(left <= right for left, right in zip(ordered, ordered[1:]))
                or names.index('authenticated') < names.index('connected')
                or names.index('accepted') < names.index('commandSent')):
            raise RuntimeError('missing actual delayed-advertising recovery evidence')
    if battery_only:
        trial = record.get('cycle', {})
        if (not standby or record.get('batteryOnlyRequested') is not True
                or trial.get('chargingAtCheckpoint') is not False
                or not isinstance(trial.get('batteryPercent'), int) or trial['batteryPercent'] <= 10):
            raise RuntimeError('missing eligible battery-only standby evidence')
    if network_faults:
        faults = record.get('networkFaults', [])
        if len(faults) != 2 or [f.get('point') for f in faults] != ['receiving', 'published']:
            raise RuntimeError('missing network fault cases')
        for fault in faults:
            if (fault.get('run') != run or fault.get('state') != 4 or fault.get('sawOffline') is not True
                    or fault.get('outageMs', 0) < 5000 or fault.get('publications') != 1
                    or fault.get('passed') is not True):
                raise RuntimeError('missing actual network outage/recovery/publication evidence')
        if faults[0].get('resumed', 0) < 65536:
            raise RuntimeError('missing upload prefix resume')
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


def collect_x4_artifacts(destination, captures):
    manifest = json.loads((destination / 'manifest.json').read_text())
    attachments = [item for test in manifest for item in test['attachments']]
    for capture in captures:
        matches = [item for item in attachments
                   if item['suggestedHumanReadableName'].startswith(capture['name'] + '_')
                   and item['exportedFileName'].endswith('.png')]
        if len(matches) != 1:
            raise RuntimeError('Missing or ambiguous X4 capture: ' + capture['name'])
        filename = matches[0]['exportedFileName']
        if Path(filename).name != filename:
            raise RuntimeError('Invalid exported attachment path')
        payload = (destination / filename).read_bytes()
        if hashlib.sha256(payload).hexdigest() != capture['sha256']:
            raise RuntimeError('X4 PNG differs from saved evidence')
        (destination / (capture['name'] + '.png')).write_bytes(payload)


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


DISCOVERY_MARKER = 'POCKET_DISCOVERY_RESULT '


def discovery_evidence(log, run):
    records = [json.loads(line.split(DISCOVERY_MARKER, 1)[1]) for line in log.splitlines()
               if DISCOVERY_MARKER in line]
    if len(records) != 1:
        raise RuntimeError('missing or duplicate app discovery evidence')
    record = records[0]
    if (record.get('passed') is not True or record.get('run') != str(run)
            or record.get('platform') != 'macOS' or record.get('identityVerified') is not True
            or record.get('staleHintUsed') is not True or record.get('port') != 80
            or not isinstance(record.get('version'), str) or '-dev-' not in record['version']):
        raise RuntimeError('invalid or stale app discovery evidence')
    private_host(record.get('host', ''))
    return record


def resolve_reader(config, config_path, discover):
    """Treat an IP as a hint, never as enrollment. Persist only after a fresh ID check."""
    host, identity = private_host(config['host']), config['deviceID']
    if not isinstance(identity, str) or not identity:
        raise ValueError('missing enrolled reader identity; enroll once before running')
    try:
        status = reader_status(host, identity)
        return host, status, {'method': 'saved-address', 'identityVerified': True}
    except (OSError, ValueError, pocket_put.ProtocolError):
        # A stale address may now belong to an unrelated reader. The real app
        # filters candidates by the enrolled identity before accepting a session.
        pass
    evidence = discover()
    new_host = private_host(evidence['host'])
    status = reader_status(new_host, identity, evidence['version'])
    updated = {**config, 'host': new_host}
    if new_host != host:
        save(config_path, updated)
        config.update(updated)
    return new_host, status, {'method': 'app-discovery', 'addressChanged': new_host != host,
                              'identityVerified': True, 'app': evidence}


def discover_with_app(job, base, app, config):
    run_id = secrets.randbelow(0xffffffff) + 1
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(('TEST_RUNNER_POCKET_HARDWARE_', 'POCKET_HARDWARE_'))}
    for key, value in {'ENABLED': '1', 'DISCOVERY': '1', 'HOST': config['host'],
                       'DEVICE_ID': config['deviceID'], 'RUN': str(run_id)}.items():
        env['TEST_RUNNER_POCKET_HARDWARE_' + key] = value
    log = job.command('app-reader-discovery', base + ['test-without-building',
        '-parallel-testing-enabled', 'NO',
        '-only-testing:PocketMacTests/ReaderHardwareTests/testDiscoverEnrolledReader',
        '-resultBundlePath', str(job.output / 'app-reader-discovery.xcresult')],
        cwd=app, env=env, timeout=360)
    record = discovery_evidence(log.read_text(), run_id)
    require_companion_stopped()
    return record


def x4_continuity_argument_error(args):
    if not getattr(args, 'x4_continuity', False):
        return None
    if args.action != 'run' or not args.firmware:
        return '--x4-continuity requires run and --firmware for the already installed developer artifact'
    conflicts = ('host', 'flash', 'install_only', 'build_firmware', 'checks', 'ios_simulator', 'ios_device',
                 'x4_recover_empty_revision', 'x4_recover_from', 'x4_sleep', 'x4_starters',
                 'x4_layout_only', 'x4_settings', 'x4_typography_only', 'x3_only', 'wifi_setup',
                 'real_epub', 'continuity_suite', 'ble_continuity', 'screen_suite', 'interface_suite',
                 'network_faults', 'standby', 'wake_control', 'delayed_advertising', 'battery_only')
    if any(getattr(args, name, False) for name in conflicts):
        return '--x4-continuity is a standalone Mac/X4 HTTP scenario; installation and all other scenarios are separate'
    if args.cycles != 1 or args.standby_seconds != 90 or args.cppcheck != 'build/cppcheck-source/cppcheck':
        return '--x4-continuity requires cycles=1 and default unused scenario/check options'
    return None


def install_only_argument_error(args):
    if not getattr(args, 'install_only', False):
        return None
    if args.action != 'run' or not args.flash or not args.firmware:
        return '--install-only requires run, --flash and --firmware'
    conflicts = ('host', 'x4_continuity', 'build_firmware', 'checks', 'ios_simulator', 'ios_device',
                 'x4_recover_empty_revision', 'x4_recover_from', 'x4_sleep',
                 'x4_starters', 'x4_layout_only', 'x4_settings', 'x4_typography_only',
                 'x3_only', 'wifi_setup', 'real_epub', 'continuity_suite',
                 'ble_continuity', 'screen_suite', 'interface_suite', 'network_faults',
                 'standby', 'wake_control', 'delayed_advertising', 'battery_only')
    if any(getattr(args, name, False) for name in conflicts):
        return '--install-only excludes hardware scenarios, iOS, checks and firmware builds'
    if args.cycles != 1 or args.standby_seconds != 90 or args.cppcheck != 'build/cppcheck-source/cppcheck':
        return '--install-only requires cycles=1 and default unused scenario/check options'
    return None


def run(args, config, job):
    scope_error = x4_continuity_argument_error(args) or install_only_argument_error(args)
    if scope_error:
        raise ValueError(scope_error)
    job.stage('exclusive-app-preflight', require_companion_stopped)
    host, identity = private_host(config['host']), config['deviceID']
    app = args.app.resolve()
    job.report['sources'] = {'firmware': source_state(ROOT), 'app': source_state(app),
                             'sdk': source_state(ROOT / 'freeink-sdk')}
    if getattr(args, 'install_only', False):
        job.report.update(installationOnly=True, acceptancePerformed=False, hardware=[])
        if not isinstance(identity, str) or not identity:
            raise ValueError('missing enrolled reader identity; enroll once before running')
        # No app build, discovery or test can change settings before inspection.
        # A stale address fails closed; it never substitutes for enrollment.
        status = job.stage('reader-preflight', lambda: reader_status(host, identity))
        discovery = {'method': 'saved-address', 'identityVerified': True}
    else:
        job.command('app-project', ['xcodegen', 'generate'], cwd=app)
        derived = app / '.build/dev-pipeline-mac'
        base = ['xcodebuild', '-project', 'Pocket.xcodeproj', '-scheme', 'PocketMac', '-configuration', 'Debug',
                '-destination', 'platform=macOS,arch=arm64', '-derivedDataPath', str(derived), 'CODE_SIGNING_ALLOWED=YES']
        job.command('mac-build', base + ['build-for-testing'], cwd=app)
        app_binary = derived / 'Build/Products/Debug/Pocket.app/Contents/MacOS/Pocket'
        job.report['macApp'] = {'path': str(app_binary), 'sha256': hashlib.sha256(app_binary.read_bytes()).hexdigest()}
        if getattr(args, 'x4_continuity', False):
            if not isinstance(identity, str) or not identity:
                raise ValueError('missing enrolled reader identity; enroll once before running')
            status = job.stage('reader-preflight', lambda: reader_status(host, identity))
            discovery = {'method': 'saved-address', 'identityVerified': True}
        else:
            host, status, discovery = job.stage('reader-preflight', lambda: resolve_reader(
                config, args.config, lambda: discover_with_app(job, base, app, config)))
    job.report['discovery'] = discovery
    version = status['version']
    if (getattr(args, 'x4_settings', False) or getattr(args, 'x4_starters', False) or getattr(args, 'x4_sleep', False) or getattr(args, 'x4_continuity', False)) and status.get('device') != 'X4':
        raise RuntimeError('X4 settings acceptance requires an enrolled X4')
    if getattr(args, 'x3_only', False) and status.get('device') != 'X3':
        raise RuntimeError('X3-only acceptance rejected this reader before hardware mutations')
    if (args.standby or getattr(args, 'ble_continuity', False)) and status.get('device') != 'X3':
        raise RuntimeError('experimental standby hardware scenario is X3 only')
    if args.checks or args.build_firmware:
        commands = [
            ('host-configure', ['cmake', '-S', 'test', '-B', 'build/host-tests', '-DCMAKE_BUILD_TYPE=Release']),
            ('host-build', ['cmake', '--build', 'build/host-tests', '-j8']),
            ('host-tests', ['ctest', '--test-dir', 'build/host-tests', '--output-on-failure', '-j8']),
            ('pipeline-tests', [sys.executable, '-m', 'unittest', 'discover', '-s', 'scripts', '-p', 'test_*.py']),
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
    if (args.standby or getattr(args, 'ble_continuity', False)) and 'ble-standby' not in version:
        raise RuntimeError('standby scenario requires the experimental ble_standby artifact')
    job.report['version'] = version
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
    platforms = [] if getattr(args, 'install_only', False) else [('macOS', base, 'PocketMacTests')]
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
    if getattr(args, 'install_only', False):
        job.report['scope'] = 'installation-only'
        job.report['installationOnly'] = True
        job.report['acceptancePerformed'] = False
        job.report['hardwareAcceptance'] = 'not performed; installation/version verification only'
        job.report['passed'] = True
        job.flush()
        return
    for platform, command, target in platforms:
        for index in range(args.cycles):
            reserve = 20 if getattr(args, 'x4_continuity', False) else 16
            run_id = secrets.randbelow(0xffffffff - reserve) + 1  # reserve capture sequence IDs
            env = {key: value for key, value in os.environ.items() if not key.startswith('TEST_RUNNER_POCKET_HARDWARE_')}
            for key, value in {'ENABLED': '1', 'HOST': host, 'DEVICE_ID': identity, 'VERSION': version,
                               'X4_CONTINUITY': '1' if getattr(args, 'x4_continuity', False) else '0',
                               'MODEL': 'X4' if getattr(args, 'x4_continuity', False) else '',
                               'X4_SLEEP': '1' if getattr(args, 'x4_sleep', False) else '0',
                               'X4_STARTERS': '1' if getattr(args, 'x4_starters', False) else '0',
                               'X4_LAYOUT_ONLY': '1' if getattr(args, 'x4_layout_only', False) else '0',
                               'X4_SETTINGS': '1' if getattr(args, 'x4_settings', False) else '0',
                               'X4_TYPOGRAPHY_ONLY': '1' if getattr(args, 'x4_typography_only', False) else '0',
                               'RUN': str(run_id), 'STANDBY': '1' if args.standby else '0',
                               'STANDBY_SECONDS': str(getattr(args, 'standby_seconds', 90)),
                               'WAKE_CONTROL': '1' if getattr(args, 'wake_control', False) else '0',
                               'DELAYED_ADVERTISING': '1' if getattr(args, 'delayed_advertising', False) else '0',
                               'BATTERY_ONLY': '1' if getattr(args, 'battery_only', False) else '0',
                               'CONTINUITY': '1' if getattr(args, 'continuity_suite', False) and platform == 'macOS' else '0',
                               'BLE_CONTINUITY': '1' if getattr(args, 'ble_continuity', False) and platform == 'macOS' else '0',
                               'SCREENS': '1' if getattr(args, 'screen_suite', False) and platform == 'macOS' else '0',
                               'NETWORK_FAULTS': '1' if getattr(args, 'network_faults', False) else '0',
                               'WIFI_SETUP': '1' if getattr(args, 'wifi_setup', False) else '0',
                               'INTERFACE': '1' if getattr(args, 'interface_suite', False) else '0'}.items():
                env['TEST_RUNNER_POCKET_HARDWARE_' + key] = value
            if getattr(args, 'x4_recover_empty_revision', None):
                env['TEST_RUNNER_POCKET_HARDWARE_X4_EMPTY_REVISION'] = args.x4_recover_empty_revision
            if getattr(args, 'x4_recover_from', None):
                env['TEST_RUNNER_POCKET_HARDWARE_X4_RECOVERY_ROOT'] = args.x4_recover_from
            if getattr(args, 'real_epub', False) and platform == 'macOS':
                env['TEST_RUNNER_POCKET_HARDWARE_CONTINUITY_EPUB'] = str(ROOT / 'test/reading_progress/fixtures/frankenstein-se.epub')
            label = f'{platform}-hardware-{index + 1}'
            try:
                log = job.command(label, command + ['test-without-building', '-parallel-testing-enabled', 'NO',
                    '-only-testing:' + target + ('/ReaderX4SettingsTests/testBidirectionalReading' if getattr(args, 'x4_continuity', False) else '/ReaderX4SettingsTests/testActualHomeSleepAndWake' if getattr(args, 'x4_sleep', False) else '/ReaderX4SettingsTests/testStarterContentAndPresets' if getattr(args, 'x4_starters', False) else '/ReaderX4SettingsTests/testSettingsPreviewAndReboot' if getattr(args, 'x4_settings', False) else '/ReaderHardwareTests/testDeveloperConnectionCycle'),
                    '-resultBundlePath', str(job.output / (label + '.xcresult'))], cwd=app, env=env, timeout=(1800 if getattr(args, 'x4_sleep', False) else 3600 if getattr(args, 'x4_starters', False) else 1800 if getattr(args, 'x4_settings', False) else 1200 if getattr(args, 'interface_suite', False) or getattr(args, 'continuity_suite', False) or getattr(args, 'x4_continuity', False) else 480) + (getattr(args, 'standby_seconds', 90) if args.standby else 0) + (900 if getattr(args, 'ble_continuity', False) else 0))
            except RuntimeError:
                bundle = job.output / (label + '.xcresult')
                if (getattr(args, 'x4_starters', False) or getattr(args, 'x4_settings', False) or getattr(args, 'continuity_suite', False) or getattr(args, 'x4_continuity', False)) and bundle.exists():
                    destination = job.output / (label + '-continuity-failed')
                    try:
                        job.command(label + '-export-failed-continuity', ['xcrun', 'xcresulttool', 'export', 'attachments',
                            '--path', str(bundle), '--output-path', str(destination)])
                        job.report['continuityFailureArtifacts'] = str(destination)
                    except RuntimeError as export_error:
                        job.report['continuityExportError'] = str(export_error)
                    job.flush()
                raise
            evidence = job.stage(label + '-evidence', lambda: hardware_evidence(
                log.read_text(), run_id, version, args.standby, platform, getattr(args, 'interface_suite', False), getattr(args, 'wifi_setup', False), getattr(args, 'screen_suite', False) and platform == 'macOS', getattr(args, 'standby_seconds', 90), getattr(args, 'battery_only', False), getattr(args, 'delayed_advertising', False), getattr(args, 'wake_control', False), getattr(args, 'network_faults', False), (getattr(args, 'continuity_suite', False) or getattr(args, 'x4_continuity', False)) and platform == 'macOS', getattr(args, 'real_epub', False) and platform == 'macOS', getattr(args, 'ble_continuity', False) and platform == 'macOS'))
            if getattr(args, 'x4_continuity', False):
                job.stage(label + '-x4-continuity-evidence', lambda: check_x4_continuity(evidence))
            if getattr(args, 'x4_sleep', False):
                job.stage(label + '-actual-sleep-evidence', lambda: check_x4_sleep(evidence))
                destination = job.output / (label + '-x4-sleep')
                job.command(label + '-export-actual-sleep', ['xcrun', 'xcresulttool', 'export', 'attachments',
                    '--path', str(job.output / (label + '.xcresult')), '--output-path', str(destination)])
                collect_x4_artifacts(destination, evidence['x4SleepValidation']['captures'])
            if getattr(args, 'x4_starters', False):
                job.stage(label + '-starter-evidence', lambda: check_x4_starters(evidence, getattr(args, 'x4_layout_only', False)))
                destination = job.output / (label + '-x4-starters')
                job.command(label + '-export-starters', ['xcrun', 'xcresulttool', 'export', 'attachments',
                    '--path', str(job.output / (label + '.xcresult')), '--output-path', str(destination)])
                collect_x4_artifacts(destination, evidence['x4StartersValidation']['captures'])
            if getattr(args, 'x4_settings', False):
                job.stage(label + '-x4-evidence', lambda: check_x4_settings(evidence, getattr(args, 'x4_typography_only', False)))
                destination = job.output / (label + '-x4-settings')
                job.command(label + '-export-x4', ['xcrun', 'xcresulttool', 'export', 'attachments',
                    '--path', str(job.output / (label + '.xcresult')), '--output-path', str(destination)])
                collect_x4_artifacts(destination, evidence['x4SettingsValidation']['captures'])
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
            if (getattr(args, 'continuity_suite', False) or getattr(args, 'x4_continuity', False)) and platform == 'macOS':
                destination = job.output / (label + '-continuity')
                job.command(label + '-export-continuity', ['xcrun', 'xcresulttool', 'export', 'attachments',
                    '--path', str(job.output / (label + '.xcresult')), '--output-path', str(destination)])
                collect_screen_artifacts(destination, evidence['continuityValidation']['captures'])
                if getattr(args, 'x4_continuity', False):
                    check_x4_continuity_bitmaps(destination, evidence['continuityValidation']['captures'])
                evidence['continuityValidation']['exportedDirectory'] = str(destination)
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
    parser.add_argument('--install-only', action='store_true', help='install an explicit developer artifact and verify its version without building or running app hardware scenarios')
    parser.add_argument('--x4-recover-empty-revision', help='with --x4-starters, restore only a canonical empty revision already identified in prior-run evidence')
    parser.add_argument('--x4-recover-from', help='with --x4-starters, restore originals from a failed X4 app-sandbox recovery directory before retesting')
    parser.add_argument('--x4-sleep', action='store_true', help='actual X4 Home, USB-powered timer deep sleep/wake, exact preview and setting retention')
    parser.add_argument('--x4-starters', action='store_true', help='physical X4 36 bundled cards, seven presets, preview captures and reboot persistence')
    parser.add_argument('--x4-layout-only', action='store_true', help='with --x4-starters, only seven exact connected-preview/preset/reboot checks; no card timing matrix')
    parser.add_argument('--x4-continuity', action='store_true', help='standalone Mac/X4 HTTP reading continuity using the already installed explicit developer artifact')
    parser.add_argument('--x4-settings', action='store_true', help='physical X4 preferences, profile, card preview parity and reboot persistence')
    parser.add_argument('--x4-typography-only', action='store_true', help='with --x4-settings, check portrait plus independent typography controls; does not establish all rotations')
    parser.add_argument('--x3-only', action='store_true', help='reject non-X3 readers before any installation or hardware scenario')
    parser.add_argument('--wifi-setup', action='store_true', help='BLE Wi-Fi setup with nonexistent network and verified rollback')
    parser.add_argument('--real-epub', action='store_true', help='use the published Frankenstein EPUB text fixture; requires --continuity-suite')
    parser.add_argument('--continuity-suite', action='store_true', help='real bidirectional X3 reading offer acceptance and captured text verification')
    parser.add_argument('--ble-continuity', action='store_true', help='exchange every reading position over the bonded BLE standby window only; requires --continuity-suite (X3 ble_standby)')
    parser.add_argument('--screen-suite', action='store_true', help='physical X3 Home/Brief/cards/EPUB captures and actual deep-sleep validation')
    parser.add_argument('--interface-suite', action='store_true', help='full app interface scenarios with owned fixtures and restoration')
    parser.add_argument('--network-faults', action='store_true', help='actual X3 Wi-Fi outage during upload and after publication; requires --interface-suite')
    parser.add_argument('--checks', action='store_true', help='full firmware host/analysis/default-build gates')
    parser.add_argument('--cppcheck', default='build/cppcheck-source/cppcheck')
    parser.add_argument('--standby', action='store_true', help='actual app BLE wake scenario (experimental X3)')
    parser.add_argument('--wake-control', action='store_true', help='negative control: suppress only app rediscovery; never count as recovery verification')
    parser.add_argument('--delayed-advertising', action='store_true', help='inject a real reader advertising outage and require foreground rediscovery evidence')
    parser.add_argument('--battery-only', action='store_true', help='require noncharging standby checkpoint above 10 percent; emits unplug-ready marker')
    parser.add_argument('--standby-seconds', type=int, default=90, help='uninterrupted standby wait, 90..1800 seconds')
    parser.add_argument('--cycles', type=int, default=1)
    parser.add_argument('--ios-simulator', help='simulator UDID for protocol regression/build validation')
    parser.add_argument('--ios-device', help='physical iPhone UDID; already paired/permissioned Debug app required')
    args = parser.parse_args()
    scope_error = x4_continuity_argument_error(args) or install_only_argument_error(args)
    if scope_error:
        parser.error(scope_error)
    if args.x4_layout_only and not args.x4_starters:
        parser.error('--x4-layout-only requires --x4-starters')
    if args.x4_recover_empty_revision and (not args.x4_starters or args.x4_recover_from):
        parser.error('--x4-recover-empty-revision requires --x4-starters and excludes --x4-recover-from')
    if args.x4_recover_from and not args.x4_starters:
        parser.error('--x4-recover-from requires --x4-starters')
    if args.x4_sleep and (args.x4_starters or args.x4_settings or args.standby or args.screen_suite or args.interface_suite or args.ios_device or args.continuity_suite):
        parser.error('--x4-sleep is a dedicated Mac/X4 externally-powered acceptance scenario')
    if args.x4_starters and (args.x4_settings or args.standby or args.screen_suite or args.interface_suite or args.ios_device or args.continuity_suite or args.wifi_setup):
        parser.error('--x4-starters is a dedicated Mac/X4 acceptance scenario')
    if getattr(args, 'x4_settings', False) and (args.standby or args.screen_suite or args.interface_suite or args.ios_device or args.continuity_suite):
        parser.error('--x4-settings is a dedicated Mac/X4 acceptance scenario')
    if args.x4_typography_only and not args.x4_settings:
        parser.error('--x4-typography-only requires --x4-settings')
    if args.real_epub and not args.continuity_suite:
        parser.error('--real-epub requires --continuity-suite')
    if args.ble_continuity and not args.continuity_suite:
        parser.error('--ble-continuity requires --continuity-suite')
    if args.ble_continuity and args.build_firmware == 'default':
        parser.error('--ble-continuity requires --build-firmware ble_standby')
    if args.action == 'run' and args.host:
        parser.error('--host is enrollment only; use configure to change the expected reader')
    if args.network_faults and not args.interface_suite:
        parser.error('--network-faults requires --interface-suite')
    if args.wake_control and not args.delayed_advertising:
        parser.error('--wake-control requires --delayed-advertising')
    if args.delayed_advertising and not args.standby:
        parser.error('--delayed-advertising requires --standby')
    if args.battery_only and not args.standby:
        parser.error('--battery-only requires --standby')
    if args.standby and args.build_firmware == 'default':
        parser.error('--standby requires --build-firmware ble_standby')
    if not 90 <= args.standby_seconds <= 1800 or (not args.standby and args.standby_seconds != 90):
        parser.error('--standby-seconds requires --standby and must be 90..1800')
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
