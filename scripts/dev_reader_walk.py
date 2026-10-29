#!/usr/bin/env python3
"""Private, opt-in Wi-Fi reader walk. `plan` is offline.

Requires an already installed exact developer build. Never installs firmware,
changes Wi-Fi, replays an ambiguous POST, or writes a user's reading position.
`observe` resumes a pending request and cleans only a verified successful trial.
"""
import argparse
from contextlib import redirect_stdout
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import struct
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
import zlib

import dev_pipeline
import pocket_put
import reader_benchmark

ROOT = Path(__file__).resolve().parents[1]
API = '/api/pocket/v1/'
MAX_BOOK = 4 * 1024 * 1024
MAX_RESPONSE = 1024 * 1024
MAX_ERROR_BODY = 256
# Exact fixed server responses only. Queries (file names/run/identity) never enter
# diagnostics, and an unknown response body is not safe to print even when short.
SAFE_HTTP_PATHS = frozenset(['/api/status'] + [API + p for p in (
    'files', 'files/content', 'commit', 'reading', 'preferences', 'profile',
    'content/state', 'dev/build-failure', 'dev/reader-capture', 'dev/screen-capture')])
DRAWING_GET_MESSAGES = {
    API + 'dev/reader-capture': b'Reader is drawing',
    API + 'files/content': b'Reader is drawing; retry the download',
}
SAFE_HTTP_BODIES = frozenset(DRAWING_GET_MESSAGES.values()) | frozenset(x.encode() for x in (
    'Another file transfer is active', 'Reader identity mismatch', 'Requires idle reader',
    'Reader memory is too low for content verification',
    'Reader memory is too low for the download right now', 'SD card is unavailable',
    'Missing reader fixture or request could not be saved',
    'Library preflight or checked admission failed; preserve evidence'))
TERMINAL = {'completed', 'build-failed', 'timed-out', 'interrupted', 'end-of-book', 'capture-failed'}


class WalkError(Exception):
    """A bounded, privacy-safe error suitable for the final report."""


class HTTPFailure(WalkError):
    def __init__(self, code, *, method=None, path=None, body=b'', attempts=1, body_complete=True):
        self.code = code
        self.method = method if method in ('GET', 'POST', 'PUT', 'DELETE', 'HEAD') else 'unknown-method'
        self.path = path if path in SAFE_HTTP_PATHS else 'unknown-endpoint'
        self.attempts = attempts
        self.body_bytes_read = min(len(body), MAX_ERROR_BODY + 1)
        self.body_truncated = len(body) > MAX_ERROR_BODY
        self.body_complete = body_complete and not self.body_truncated
        self.body = body.decode('ascii') if self.body_complete and body in SAFE_HTTP_BODIES else '[redacted]'
        super().__init__(f'{self.method} {self.path}: HTTP {code}; body={self.body}; '
                         f'bodyBytesRead={self.body_bytes_read}; bodyComplete={self.body_complete}; '
                         f'attempts={attempts}; no mutation was replayed')


def require(condition, message):
    if not condition:
        raise WalkError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def save_bytes(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    temporary = path.with_name(path.name + '.tmp')
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, 'wb') as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)
    os.chmod(path, 0o600)


def save_json(path, value):
    save_bytes(path, (json.dumps(value, indent=2, sort_keys=True) + '\n').encode())


def load_json(path):
    return json.loads(Path(path).read_text())


def valid_plan(plan):
    require(plan.get('schema') == 'pocket-dev-reader-walk-v1', 'Unknown plan schema')
    require(plan.get('model') in ('X3', 'X4'), 'Explicit X3 or X4 model required')
    require(type(plan.get('pages')) is int and 1 <= plan['pages'] <= 128, 'pages must be 1..128')
    require('-dev-' in plan.get('expectedVersion', ''), 'An exact developer version is required; production refused')
    require(re.fullmatch(r'[A-Za-z0-9]{8}', plan.get('deviceID', '')) is not None, 'Enrolled identity required')
    dev_pipeline.private_host(plan['host'])
    require(isinstance(plan.get('fixtures'), list) and 1 <= len(plan['fixtures']) <= 8, 'One to eight fixtures required')
    runs = set()
    for fixture in plan['fixtures']:
        run = fixture.get('run')
        require(type(run) is int and 1 <= run <= 0xffffffff and run not in runs, 'Fresh, distinct nonzero uint32 runs required')
        runs.add(run)
        require(re.fullmatch(r'[A-Za-z0-9_-]{1,32}', fixture.get('label', '')) is not None
                and fixture.get('local') == fixture['label'] + '.epub', 'Unsafe local fixture path')
        require(fixture.get('target') == f'/Pocket visual {run}.epub', 'Only the run-owned fixture path is allowed')
        require(re.fullmatch(r'/\.pocket-[a-f0-9]{8}(?:-[a-f0-9]{4}){3}-[a-f0-9]{12}\.part',
                            fixture.get('staging', '')) is not None, 'UUID staging required')
        require(type(fixture.get('bytes')) is int and 0 < fixture['bytes'] <= MAX_BOOK, 'Fixture size must be 1 byte..4 MiB')
        require(re.fullmatch(r'[a-f0-9]{64}', fixture.get('sha256', '')) is not None, 'Fixture SHA256 required')


def create_plan(args):
    config = load_json(args.config)
    output = args.output.resolve()
    require(not output.exists(), 'Plan directory already exists; preserve it and choose a fresh directory')
    fixtures = []
    labels = set()
    runs = set()
    for specification in args.fixture:
        label, separator, filename = specification.partition('=')
        require(separator and re.fullmatch(r'[a-zA-Z0-9_-]{1,32}', label) and label not in labels,
                'Use distinct --fixture label=/absolute/or/relative/book.epub')
        labels.add(label)
        source = Path(filename).resolve()
        require(source.suffix.lower() == '.epub', 'Fixture must be EPUB')
        require(0 < source.stat().st_size <= MAX_BOOK, 'Fixture exceeds admitted size')
        data = source.read_bytes()
        require(0 < len(data) <= MAX_BOOK and data.startswith(b'PK'), 'Fixture must be a bounded EPUB ZIP')
        run = secrets.randbelow(0xffffffff) + 1
        while run in runs:
            run = secrets.randbelow(0xffffffff) + 1
        runs.add(run)
        fixtures.append({'label': label, 'source': str(source), 'local': f'{label}.epub', 'bytes': len(data),
                         'sha256': digest(data), 'run': run, 'target': f'/Pocket visual {run}.epub',
                         'staging': f'/.pocket-{uuid.uuid4()}.part'})
    plan = {'schema': 'pocket-dev-reader-walk-v1', 'createdUTC': datetime.now(timezone.utc).isoformat(),
            'host': config['host'], 'deviceID': config['deviceID'], 'expectedVersion': args.expected_version,
            'model': args.model, 'pages': args.pages, 'fixtures': fixtures,
            'scope': 'Developer firmware only. Rendered pages/captured bytes; no optical, button, or production acceptance.',
            'cacheScope': 'Each unchanged byte copy has a fresh run path/cold path-keyed cache. Original book paths/caches are neither reused nor cleaned. A developer PASS does not resolve the prior production failure.',
            'backupLimit': 'Snapshots are public API observations, not full SD restore. Existing raw-state backup acknowledgement required.',
            'transferLimit': 'Existing POCKET-PUT may sweep stale .pocket-*.part files in root. No competing/unrecovered transfer may exist.',
            'deviceIOPerformed': False}
    valid_plan(plan)
    output.mkdir(parents=True, mode=0o700)
    os.chmod(output, 0o700)
    for fixture in fixtures:
        data = Path(fixture['source']).read_bytes()
        require(digest(data) == fixture['sha256'], 'Source changed while freezing plan')
        save_bytes(output / fixture['local'], data)
    save_json(output / 'plan.json', plan)
    return output / 'plan.json'


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        # The caller adds only its original method/fixed path, never the URL.
        raise urllib.error.HTTPError(req.full_url, code, 'Redirect refused', headers, fp)


class Client:
    def __init__(self, plan, *, opener=None, clock=time.monotonic, sleep=time.sleep):
        self.plan = plan
        self.opener = opener or urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())
        self.clock, self.sleep = clock, sleep

    def request(self, method, path, query=None, body=None, expected=200, limit=MAX_RESPONSE):
        params = dict(query or {})
        if path != '/api/status':
            params['deviceID'] = self.plan['deviceID']
        url = f"http://{self.plan['host']}{path}"
        if params:
            url += '?' + urllib.parse.urlencode(params)
        data = json.dumps(body).encode() if body is not None else (b'' if method == 'POST' else None)
        request = urllib.request.Request(url, data=data, method=method,
                                         headers={'Content-Type': 'application/json', 'Connection': 'close'})
        retry_body = DRAWING_GET_MESSAGES.get(path) if method == 'GET' and expected == 200 and body is None else None
        deadline = self.clock() + 10 if retry_body is not None else None
        attempts = 0
        last_failure = None
        while True:
            remaining = deadline - self.clock() if deadline is not None else 12
            if remaining <= 0:
                if last_failure is not None:
                    raise last_failure from None
                raise WalkError('Diagnostic GET read budget expired before sending')
            attempts += 1
            try:
                response = self.opener.open(request, timeout=min(12, remaining))
                http_error = False
            except urllib.error.HTTPError as error:
                response, http_error = error, True
            # Transport errors and cancellation escape without replay. Closing
            # every HTTP error before backoff releases the socket first.
            with response:
                if http_error or response.status != expected:
                    complete = True
                    try:
                        error_body = response.read(MAX_ERROR_BODY + 1)
                    except (OSError, urllib.error.URLError):
                        error_body, complete = b'', False
                    last_failure = HTTPFailure(response.status, method=method, path=path, body=error_body,
                                               attempts=attempts, body_complete=complete)
                else:
                    content = response.read(limit + 1)
                    require(len(content) <= limit, 'Response exceeds bounded size')
                    headers = dict(response.headers.items())
                    if deadline is not None and self.clock() >= deadline:
                        raise WalkError(f'GET {path}: read budget expired after {attempts} attempts; no mutation was replayed')
                    return content, headers
            if (retry_body is None or last_failure.code != 409 or not last_failure.body_complete
                    or error_body != retry_body or attempts >= 6 or self.clock() + 1 >= deadline):
                raise last_failure from None
            self.sleep(1)

    def get_json(self, path, query=None):
        data, _ = self.request('GET', path, query)
        try:
            value = json.loads(data)
        except (ValueError, UnicodeError):
            raise WalkError('Malformed JSON response') from None
        require(isinstance(value, dict), 'JSON object required')
        return value

    def upload(self, status, data, path):
        # Exactly one bulk attempt; ambiguous staging is retained for inspection.
        return pocket_put.upload_once(self.plan['host'], status, data, path, False, 0)


def verify_status(status, plan):
    require(status.get('deviceID') == plan['deviceID'], 'Reader identity mismatch')
    require(status.get('version') == plan['expectedVersion'] and '-dev-' in status.get('version', ''),
            'Exact developer build required; production or different build refused')
    require(status.get('device') == plan['model'] and status.get('mode') == 'STA', 'Exact model and Same Wi-Fi required')
    require(all(type(status.get(key)) is int and status[key] == expected for key, expected in
                (('firmwareLineage', 2), ('readerFiles', 2), ('readingProgress', 1))),
            'Current reader file/reading contract required')
    require(type(status.get('uploadStreamPort')) is int and 1 <= status['uploadStreamPort'] <= 65535,
            'Upload stream unavailable')
    require(status.get('transferState', {}).get('phase') in ('idle', 'saved', 'removed'),
            'Reader transfer must be idle before this operation')
    return status


def parse_build_failure(data):
    require(len(data) == 96 and data[:5] == b'PDBF\x01' and data[5] < 8 and data[91] == 0,
            'Invalid build-failure diagnostic format')
    require(struct.unpack_from('<I', data, 92)[0] == zlib.crc32(data[:92]), 'Build-failure diagnostic CRC mismatch')
    names = ('none', 'html-stream', 'section-file', 'build-context', 'parser', 'begin-parse', 'layout', 'commit')
    return {'sha256': digest(data), 'step': data[5], 'stepName': names[data[5]], 'detail': data[6],
            'spine': struct.unpack_from('<H', data, 8)[0], 'count': struct.unpack_from('<H', data, 10)[0],
            'bookPathHash': struct.unpack_from('<I', data, 12)[0],
            'freeHeap': struct.unpack_from('<I', data, 16)[0], 'largestBlock': struct.unpack_from('<I', data, 20)[0],
            'uptimeSec': struct.unpack_from('<I', data, 24)[0],
            'version': data[28:92].split(b'\0', 1)[0].decode('ascii', errors='replace')}


def diagnostic(client, run=None):
    try:
        data, headers = client.request('GET', API + 'dev/build-failure', {} if run is None else {'run': run}, limit=96)
    except HTTPFailure as error:
        if error.code == 404:
            return None, None
        raise
    headers = {key.lower(): value for key, value in headers.items()}
    require(headers.get('x-build-failure-run') == str(run or 0), 'Build-failure diagnostic run mismatch')
    return parse_build_failure(data), data


def inventory(client, identity):
    rows = []
    cursor = 0
    visited = set()
    for _ in range(4096):
        require(cursor not in visited, 'Inventory cursor repeated')
        visited.add(cursor)
        result = client.get_json(API + 'files', {'path': '/', 'cursor': cursor})
        require(result.get('deviceID') == identity and result.get('path') == '/', 'Inventory identity/path mismatch')
        page = result.get('entries')
        require(isinstance(page, list) and len(page) <= 12, 'Invalid inventory page')
        for item in page:
            require(isinstance(item, dict) and isinstance(item.get('name'), str)
                    and '/' not in item['name'] and type(item.get('directory')) is bool
                    and type(item.get('size')) is int and item['size'] >= 0, 'Invalid inventory entry')
            rows.append(item)
        cursor = result.get('nextCursor')
        require(type(cursor) is int and 0 <= cursor <= 8 * 1024 * 1024 and cursor % 32 == 0, 'Invalid inventory cursor')
        if cursor == 0:
            require(len({x['name'] for x in rows}) == len(rows), 'Inventory contains duplicate names')
            return sorted(rows, key=lambda item: item['name'])
    raise WalkError('Inventory exceeded bounded scan')


def snapshot(client, plan):
    verify_status(client.get_json('/api/status'), plan)
    result = {'inventory': inventory(client, plan['deviceID'])}
    for name in ('reading', 'profile', 'preferences', 'content/state'):
        value = client.get_json(API + name)
        if name != 'preferences':
            require(value.get('deviceID') == plan['deviceID'], 'Snapshot identity mismatch')
        result[name] = value
    record, raw = diagnostic(client)
    result['buildFailure'] = record
    result['buildFailureRawHex'] = raw.hex() if raw is not None else None
    return result


def download_book(client, fixture):
    data = bytearray()
    while len(data) < fixture['bytes']:
        piece, _ = client.request('GET', API + 'files/content',
                                  {'path': fixture['target'], 'size': fixture['bytes'], 'offset': len(data)}, limit=4096)
        require(0 < len(piece) <= min(4096, fixture['bytes'] - len(data)), 'Invalid book download piece')
        data.extend(piece)
    require(digest(data) == fixture['sha256'], 'Reader fixture bytes differ from unchanged source')
    return bytes(data)


def readiness(client, run):
    result = client.get_json(API + 'dev/reader-capture', {'run': run})
    walk = result.get('walk')
    require(type(result.get('walkVersion')) is int and result['walkVersion'] == 1
            and type(result.get('maxWalkPages')) is int and result['maxWalkPages'] == 128
            and isinstance(walk, dict) and type(walk.get('protocolVersion')) is int and walk['protocolVersion'] == 1
            and walk.get('historyIsolated') is True, 'History-isolated walk protocol unavailable; no mutation allowed')
    return result


def fresh_readiness(result, run):
    require(all(type(result.get(key)) is int and result[key] == 0 for key in ('activeRun', 'pendingRun', 'fixtureRun')),
            'An existing developer reader operation owns the device')
    require(result['walk'].get('state') == 'absent' and result['walk'].get('run') == 0,
            'An existing walk receipt must be reviewed before another run')
    require(result.get('walkReceiptPresent') is False and type(result.get('walkReceiptRun')) is int
            and result['walkReceiptRun'] == 0, 'Existing or unknown walk receipt blocks all mutations')
    require(result.get('bookReady') is False, 'Run fixture already exists; refusing replacement')


def validate_result(walk, fixture, pages):
    require(type(walk.get('run')) is int and walk['run'] == fixture['run']
            and walk.get('state') == 'completed', 'Walk did not complete successfully')
    require(all(type(walk.get(key)) is int for key in
                ('requestedPages', 'renderedPages', 'turns', 'failureStep', 'failureDetail')),
            'Page and failure counters must be integers')
    require(walk.get('requestedPages') == pages and walk.get('renderedPages') == pages
            and walk.get('turns') == pages - 1, 'Actual rendered page/turn counts do not match request')
    require(walk.get('historyIsolated') is True and walk.get('progressMonotonic') is True
            and walk.get('captureReady') is True, 'Missing isolated monotonic render/capture evidence')
    require(walk.get('failureStep') == 0 and walk.get('failureDetail') == 0, 'Walk records a build failure')
    require(all(type(walk.get(key)) is int and walk[key] >= 0 for key in ('lastSpine', 'lastPage', 'elapsedMs')),
            'Invalid terminal page/elapsed evidence')
    require(walk['lastPage'] > 0, 'Terminal reader page is one-based')
    require(walk['elapsedMs'] <= 600000, 'Walk exceeded firmware deadline')
    require(type(walk.get('progressPercent')) in (int, float) and 0 <= walk['progressPercent'] <= 100,
            'Invalid terminal progress percentage')


def validate_image_diagnostics(state, fixture):
    """Validate optional run-bound telemetry, never image decoding or painting."""
    advertised = 'imageDiagnosticsVersion' in state
    if 'currentImageRendering' in state or advertised:
        require(type(state.get('currentImageRendering')) is int and 0 <= state['currentImageRendering'] <= 2,
                'Invalid current image-rendering mode; preserving diagnostics')
    if advertised:
        require(type(state['imageDiagnosticsVersion']) is int and state['imageDiagnosticsVersion'] == 1,
                'Unknown image diagnostic protocol; preserving diagnostics')
    if 'imageDiagnostics' not in state and not advertised:
        return 'not-reported'
    record = state.get('imageDiagnostics')
    require(isinstance(record, dict), 'Missing or corrupt terminal image diagnostics; preserving run')
    require(type(record.get('version')) is int and record['version'] == 1,
            'Unknown image diagnostic record version')
    require(type(record.get('run')) is int and record['run'] == fixture['run'],
            'Image diagnostic run mismatch')
    require(record.get('complete') is True, 'Image diagnostics did not finish durably; preserving run')
    require(type(record.get('mode')) is int and 0 <= record['mode'] <= 2,
            'Invalid run image-rendering mode')
    counters = ('seen', 'probeStreamFailures', 'dimensionFailures', 'extractAttempts', 'extractFailures',
                'altFallbacks', 'imageBlocks', 'beforeProbeFreeHeap', 'beforeProbeLargestBlock')
    require(all(type(record.get(key)) is int and 0 <= record[key] <= 0xffffffff for key in counters),
            'Invalid image diagnostic counters')
    require(all(type(record.get(key)) is int and 0 <= record[key] <= 9
                for key in ('lastProbeError', 'lastExtractError')), 'Invalid image diagnostic error code')
    return 'run-bound-metadata-only'


def validate_paint_diagnostics(state, fixture):
    """Validate a durable paint observation, never successful displayed pixels."""
    advertised = 'paintDiagnosticsVersion' in state
    if advertised:
        require(type(state['paintDiagnosticsVersion']) is int and state['paintDiagnosticsVersion'] == 1,
                'Unknown paint diagnostic protocol; preserving diagnostics')
    if 'paintDiagnostics' not in state and not advertised:
        return 'not-reported'
    record = state.get('paintDiagnostics')
    require(isinstance(record, dict), 'Missing or corrupt terminal paint diagnostics; preserving run')
    require(type(record.get('version')) is int and record['version'] == 1,
            'Unknown paint diagnostic record version')
    require(type(record.get('run')) is int and record['run'] == fixture['run'],
            'Paint diagnostic run mismatch')
    require(record.get('complete') is True, 'Paint diagnostics did not finish durably; preserving run')
    counters = ('renderCalls', 'cacheHits', 'lazyExtractAttempts', 'lazyExtractFailures',
                'decoderAttempts', 'decoderSuccesses', 'placeholderCalls', 'suppressedCalls',
                'pngAttempts', 'pngEntryFreeHeap', 'pngEntryLargestBlock', 'pngObjectBytes', 'pngHeapGateBytes',
                'firstFailureFreeHeap', 'firstFailureLargestBlock')
    require(all(type(record.get(key)) is int and 0 <= record[key] <= 0xffffffff for key in counters),
            'Invalid paint diagnostic counters')
    relations = (('cacheHits', 'renderCalls'), ('lazyExtractAttempts', 'renderCalls'),
                 ('lazyExtractFailures', 'lazyExtractAttempts'), ('decoderAttempts', 'renderCalls'),
                 ('decoderSuccesses', 'decoderAttempts'), ('placeholderCalls', 'renderCalls'),
                 ('suppressedCalls', 'placeholderCalls'), ('pngAttempts', 'decoderAttempts'))
    require(all(record[child] <= record[parent] for child, parent in relations),
            'Inconsistent paint diagnostic counters')
    require(type(record.get('lastLazyExtractError')) is int and 0 <= record['lastLazyExtractError'] <= 9,
            'Invalid paint ZIP error code')
    require(type(record.get('firstFailureStage')) is int and 0 <= record['firstFailureStage'] <= 16,
            'Invalid paint failure stage')
    require(type(record.get('firstFailureCode')) is int and -(1 << 31) <= record['firstFailureCode'] < (1 << 31),
            'Invalid signed paint failure code')
    require(record['firstFailureStage'] != 0 or all(record[key] == 0 for key in
            ('firstFailureCode', 'firstFailureFreeHeap', 'firstFailureLargestBlock')),
            'Paint failure details exist without a failure stage')
    require(record['pngAttempts'] != 0 or all(record[key] == 0 for key in
            ('pngEntryFreeHeap', 'pngEntryLargestBlock', 'pngObjectBytes', 'pngHeapGateBytes')),
            'PNG entry details exist without a PNG attempt')
    return 'run-bound-metadata-only'


def download_capture(client, fixture, model):
    data = bytearray()
    total = None
    while total is None or len(data) < total:
        part, headers = client.request('GET', API + 'dev/screen-capture',
                                       {'run': fixture['run'], 'offset': len(data)}, limit=1024)
        headers = {key.lower(): value for key, value in headers.items()}
        try:
            size = int(headers.get('x-capture-size', ''))
        except ValueError:
            raise WalkError('Capture size missing') from None
        require(headers.get('x-capture-run') == str(fixture['run']) and 62 < size < 100000
                and (total is None or size == total), 'Capture run or size mismatch')
        require(len(part) == min(1024, size - len(data)), 'Incomplete capture piece')
        total = size
        data.extend(part)
    require(data[:2] == b'BM' and struct.unpack_from('<I', data, 2)[0] == len(data), 'Invalid BMP capture')
    width, height = struct.unpack_from('<ii', data, 18)
    expected = (528, 792) if model == 'X3' else (480, 800)
    require((width, abs(height)) in (expected, expected[::-1]), 'Capture dimensions do not match reader model')
    return bytes(data), {'sha256': digest(data), 'bytes': len(data), 'width': width, 'height': height}


class Walker:
    def __init__(self, plan_path, client=None, clock=time.monotonic, sleep=time.sleep):
        self.plan_path = Path(plan_path).resolve()
        self.root = self.plan_path.parent
        require(self.root.stat().st_mode & 0o077 == 0, 'Plan directory must be private (mode 0700)')
        self.plan = load_json(self.plan_path)
        valid_plan(self.plan)
        for fixture in self.plan['fixtures']:
            local = self.root / fixture['local']
            require(local.is_file() and local.stat().st_size == fixture['bytes']
                    and digest(local.read_bytes()) == fixture['sha256'], 'Frozen plan fixture changed')
        self.client = client or Client(self.plan)
        self.clock, self.sleep = clock, sleep

    def persist(self, folder, report):
        save_json(folder / 'report.json', report)

    def observe(self, fixture, folder, report):
        require(report.get('submitted') is True, 'No durable one-shot submission marker; do not repeat POST')
        deadline = self.clock() + 720
        while self.clock() < deadline:
            self.sleep(3)
            try:
                verify_status(self.client.get_json('/api/status'), self.plan)
                state = readiness(self.client, fixture['run'])
            except (OSError, urllib.error.URLError):
                continue
            except HTTPFailure as error:
                if error.code in (409, 503):
                    continue
                raise
            walk = state['walk']
            require(type(walk.get('run')) is int and walk['run'] in (0, fixture['run']),
                    'Foreign walk receipt; refusing further work')
            if walk.get('run') == fixture['run'] and walk.get('state') in TERMINAL:
                # Keep additive endpoint evidence before DELETE can erase the
                # run. Unknown fields are preserved for later offline review.
                terminal = folder / 'terminal-readiness.json'
                save_json(terminal, state)
                terminal_bytes = terminal.read_bytes()
                report['terminalReadiness'] = {'file': terminal.name, 'bytes': len(terminal_bytes),
                                               'sha256': digest(terminal_bytes)}
                for key in ('currentImageRendering', 'imageDiagnosticsVersion', 'imageDiagnostics',
                            'currentReaderFontPoints', 'paintDiagnosticsVersion', 'paintDiagnostics'):
                    if key in state:
                        report[key] = state[key]
                report['imageValidation'] = 'not-performed'
                report['imageVerificationPending'] = True
                report['paintValidation'] = 'not-performed'
                report['paintVerificationPending'] = True
                report['walk'] = walk
                self.persist(folder, report)
                return state
        raise WalkError('No matching terminal receipt after bounded Same Wi-Fi observation; POST was not repeated')

    def finish(self, fixture, folder, report):
        state = self.observe(fixture, folder, report)
        walk = state['walk']
        record, raw = diagnostic(self.client, fixture['run'])
        report['walkBuildFailure'] = record
        if raw is not None:
            save_bytes(folder / 'walk-build-failure.bin', raw)
        self.persist(folder, report)
        # A failure screenshot is useful evidence too; never promote it to a pass.
        if walk.get('captureReady') is True:
            data, capture = download_capture(self.client, fixture, self.plan['model'])
            save_bytes(folder / 'final-page.bmp', data)
            report['capture'] = capture
        after = snapshot(self.client, self.plan)
        save_json(folder / 'after-walk.json', after)
        baseline = load_json(folder / 'before.json')
        without_owned = dict(after, inventory=[x for x in after['inventory'] if '/' + x['name'] != fixture['target']])
        report.update(publicStateUnchangedBeforeCleanup=without_owned == baseline, stage='terminal-evidence-saved')
        self.persist(folder, report)
        # Even malformed telemetry retains its original response, PDBF and BMP.
        # A walk's page count does not validate image blocks or actual pixels.
        report['imageDiagnosticsValidation'] = validate_image_diagnostics(state, fixture)
        report['paintDiagnosticsValidation'] = validate_paint_diagnostics(state, fixture)
        validate_result(walk, fixture, self.plan['pages'])
        if state.get('walkPerformanceVersion') == 1 and self.plan['pages'] <= reader_benchmark.MAX_PAGES:
            raw, headers = self.client.request('GET', API + 'dev/reader-capture',
                                               {'run': fixture['run'], 'performance': 1},
                                               limit=reader_benchmark.MAX_BYTES)
            save_bytes(folder / 'performance.bin', raw)
            headers = {key.lower(): value for key, value in headers.items()}
            require(headers.get('x-reader-performance-run') == str(fixture['run']),
                    'Foreign performance response; preserving evidence')
            try:
                records = reader_benchmark.decode(raw, fixture['run'], self.plan['pages'])
            except ValueError as error:
                raise WalkError(str(error)) from error
            report['performance'] = {'file': 'performance.bin', 'sha256': digest(raw),
                                     'records': records}
            self.persist(folder, report)
        require(record is None, 'Run-bound build failure exists; preserving fixture/cache/receipt')
        require(without_owned == baseline, 'Original public state changed; preserve all recovery artifacts')
        download_book(self.client, fixture)
        report.update(sourceBytesVerified=True, publicStateUnchangedBeforeCleanup=True,
                      stage='evidence-saved-before-cleanup')
        self.persist(folder, report)
        # Only an attested successful walk reaches cleanup. Each mutation is one-shot.
        verify_status(self.client.get_json('/api/status'), self.plan)
        report['cleanupStarted'] = True
        self.persist(folder, report)
        self.client.request('DELETE', API + 'dev/screen-capture', {'run': fixture['run']}, expected=204)
        self.client.request('DELETE', API + 'dev/reader-capture', {'run': fixture['run']}, expected=204)
        body, _ = self.client.request('DELETE', API + 'files',
                                      {'path': fixture['target'], 'size': fixture['bytes'], 'cursor': 0})
        removed = json.loads(body)
        require(removed.get('deviceID') == self.plan['deviceID'] and removed.get('deleted') is True,
                'Owned file deletion not verified')
        final = snapshot(self.client, self.plan)
        save_json(folder / 'after-cleanup.json', final)
        require(final == baseline, 'Public state differs after cleanup; preserve recovery artifacts')
        report.update(stage='complete', passed=True, ownedFileCleaned=True, publicStateUnchanged=True,
                      fullSDRestoreVerified=False, productionAcceptance=False)
        self.persist(folder, report)
        return report

    def execute_fixture(self, fixture):
        folder = self.root / fixture['label']
        require(not folder.exists(), 'Fixture run already started; use observe, never repeat run')
        folder.mkdir(mode=0o700)
        report = {'run': fixture['run'], 'passed': False, 'stage': 'preflight', 'submitted': False,
                  'sourceSHA256': fixture['sha256'], 'requestedPages': self.plan['pages'],
                  'productionAcceptance': False}
        self.persist(folder, report)
        try:
            data = (self.root / fixture['local']).read_bytes()
            require(len(data) == fixture['bytes'] and digest(data) == fixture['sha256'], 'Frozen fixture changed')
            status = verify_status(self.client.get_json('/api/status'), self.plan)
            fresh_readiness(readiness(self.client, fixture['run']), fixture['run'])
            before = snapshot(self.client, self.plan)
            require(not any('/' + x['name'] == fixture['target'] for x in before['inventory']),
                    'Reader fixture conflict; no overwrite allowed')
            save_json(folder / 'before.json', before)
            report['stage'] = 'upload-started'
            self.persist(folder, report)
            status = verify_status(self.client.get_json('/api/status'), self.plan)
            self.client.upload(status, data, fixture['staging'])
            report['stage'] = 'commit-submission-prepared'
            self.persist(folder, report)
            # Observe a lost publication response by complete byte readback, never another commit.
            body = {'staging': fixture['staging'], 'target': fixture['target'], 'size': len(data),
                    'crc32': f'{zlib.crc32(data):08X}'}
            try:
                self.client.request('POST', API + 'commit', body=body)
            except (OSError, urllib.error.URLError):
                report['commitReplyLost'] = True
            verify_status(self.client.get_json('/api/status'), self.plan)
            download_book(self.client, fixture)
            ready = readiness(self.client, fixture['run'])
            require(ready.get('bookReady') is True and ready['walk'].get('state') == 'absent'
                    and type(ready['walk'].get('run')) is int and ready['walk']['run'] == 0,
                    'Fixture admission or fresh walk state missing')
            require(ready.get('walkReceiptPresent') is False and ready.get('walkReceiptRun') == 0,
                    'Another walk receipt appeared after upload')
            require(all(type(ready.get(key)) is int and ready[key] == 0 for key in ('activeRun', 'pendingRun', 'fixtureRun')),
                    'Another reader operation appeared after upload')
            report.update(stage='walk-submission-prepared', submitted=True)
            self.persist(folder, report)  # crash here is ambiguous; observe is the only recovery
            try:
                self.client.request('POST', API + 'dev/reader-capture',
                                    {'run': fixture['run'], 'action': 'walk', 'pages': self.plan['pages']}, expected=202)
            except (OSError, urllib.error.URLError):
                report['walkReplyLost'] = True
            self.persist(folder, report)
            return self.finish(fixture, folder, report)
        except BaseException as error:
            report.update(passed=False, errorType=type(error).__name__, stageAtFailure=report['stage'],
                          error=str(error) if isinstance(error, WalkError) else 'Interrupted/transport failure; inspect private artifacts',
                          remoteArtifactsPreserved=not report.get('cleanupStarted', False),
                          cleanupMayBePartial=report.get('cleanupStarted', False),
                          remainingRemoteArtifactsPreserved=True, localRecoveryEvidencePreserved=True)
            self.persist(folder, report)
            raise


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    plan = sub.add_parser('plan', help='Offline: freeze unchanged fixtures and private plan; no network')
    plan.add_argument('--config', type=Path, required=True)
    plan.add_argument('--expected-version', required=True)
    plan.add_argument('--model', choices=('X3', 'X4'), required=True)
    plan.add_argument('--pages', type=int, default=20)
    plan.add_argument('--fixture', action='append', required=True, help='label=source.epub')
    plan.add_argument('--output', type=Path, required=True)
    for name in ('run', 'observe'):
        command = sub.add_parser(name, help='Submit new trials' if name == 'run' else
                                 'Observe pending one-shot request; clean only a verified success')
        command.add_argument('--plan', type=Path, required=True)
        command.add_argument('--execute', action='store_true', help='Explicit device I/O opt-in')
        if name == 'run':
            command.add_argument('--backup-receipt', type=Path, required=True)
            command.add_argument('--backup-confirmed', action='store_true')
            command.add_argument('--no-unrecovered-transfers', action='store_true',
                                 help='Acknowledge no other transfer or stale staging needs preservation')
        else:
            command.add_argument('--label', required=True)
    args = parser.parse_args(argv)
    try:
        if args.action == 'plan':
            print(create_plan(args))
            return 0
        require(args.execute, 'Device I/O requires --execute; use plan for offline preparation')
        walker = Walker(args.plan)
        with dev_pipeline.exclusive(ROOT / 'build/dev-pipeline.lock'):
            dev_pipeline.require_companion_stopped()
            log = walker.root / 'execution-private.log'
            fd = os.open(log, os.O_CREAT | os.O_WRONLY | os.O_APPEND, 0o600)
            with os.fdopen(fd, 'a') as stream, redirect_stdout(stream):
                if args.action == 'run':
                    require(args.backup_confirmed and args.no_unrecovered_transfers,
                            'Existing raw-state backup and exclusive clean transfer state must be acknowledged')
                    backup = args.backup_receipt.read_bytes()
                    require(0 < len(backup) <= MAX_RESPONSE, 'Backup receipt missing or too large')
                    json.loads(backup)
                    save_bytes(walker.root / 'external-backup-receipt.json', backup)
                    save_json(walker.root / 'authorization.json', {'planSHA256': digest(args.plan.read_bytes()),
                              'backupReceiptSHA256': digest(backup), 'backupConfirmed': True,
                              'noUnrecoveredTransfers': True, 'fullSDBackupInferred': False})
                    for fixture in walker.plan['fixtures']:
                        walker.execute_fixture(fixture)
                else:
                    authorization = load_json(walker.root / 'authorization.json')
                    require(authorization.get('planSHA256') == digest(args.plan.read_bytes())
                            and authorization.get('backupConfirmed') is True,
                            'Original authorized plan changed or backup acknowledgement missing')
                    fixtures = [f for f in walker.plan['fixtures'] if f['label'] == args.label]
                    require(len(fixtures) == 1, 'Unknown fixture label')
                    fixture = fixtures[0]
                    folder = walker.root / fixture['label']
                    report = load_json(folder / 'report.json')
                    require(report.get('run') == fixture['run'] and report.get('stage') == 'walk-submission-prepared',
                            'Observe only a pending one-shot walk; do not repeat completed or partially cleaned work')
                    try:
                        walker.finish(fixture, folder, report)
                    except BaseException as error:
                        report.update(passed=False, errorType=type(error).__name__, stageAtFailure=report['stage'],
                                      error=str(error) if isinstance(error, WalkError) else 'Observation interrupted',
                                      remoteArtifactsPreserved=not report.get('cleanupStarted', False),
                                      cleanupMayBePartial=report.get('cleanupStarted', False),
                                      remainingRemoteArtifactsPreserved=True, localRecoveryEvidencePreserved=True)
                        walker.persist(folder, report)
                        raise
        print(f'Private walk evidence: {walker.root}')
        return 0
    except (WalkError, OSError, ValueError, KeyError, urllib.error.URLError) as error:
        print(str(error) if isinstance(error, WalkError) else 'Walk stopped; inspect the private plan/report. No mutation was replayed.', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
