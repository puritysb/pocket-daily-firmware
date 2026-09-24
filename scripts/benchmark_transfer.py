#!/usr/bin/env python3
"""Compare existing HTTP and POCKET-PUT on one reader; never install firmware.

Creates uniquely named inert test data, verifies publication and downloaded SHA,
then deletes only the published test file. Failed hidden staging is reported.
Only run with the reader in File Transfer / Join a Network, no other clients.
"""
import argparse
import hashlib
import http.client
import ipaddress
import json
import socket
import time
import urllib.parse
import uuid

import pocket_put as put


PAYLOAD_PATTERN = 'shake256-pocket-bench-v1'


def benchmark_payload(size):
    """Reproducible nonrepeating blocks; never use firmware or personal data."""
    if type(size) is not int or not 1 <= size <= 8 * 1024 * 1024:
        raise ValueError('benchmark size must be 1..8388608 bytes')
    return hashlib.shake_256(b'pocket-transfer-benchmark-v1').digest(size)


def read_reader_stats(host):
    """Bounded, whitelisted developer record; never copy arbitrary reader JSON."""
    with put.urllib.request.urlopen(f'http://{host}/api/pocket/v1/dev/transfer-stats', timeout=3) as response:
        body = response.read(2049)
    if len(body) > 2048:
        raise put.ProtocolError('reader statistics exceed 2048 bytes')
    try:
        value = json.loads(body)
    except (ValueError, UnicodeDecodeError) as error:
        raise put.ProtocolError('malformed reader statistics') from error
    if not isinstance(value, dict):
        raise put.ProtocolError('reader statistics must be an object')
    scalar_fields = ('schema', 'attempt', 'outcome', 'expected', 'resumed', 'socketBytes', 'accepted',
                     'elapsedMs', 'maxServiceGapMs', 'maxReceiveGapMs', 'minHeap', 'minBlock')
    timing_fields = ('sdWriteUs', 'sdCloseUs', 'replyWriteUs')
    uint32 = lambda number: type(number) is int and 0 <= number <= 0xFFFFFFFF
    if any(not uint32(value.get(field)) for field in scalar_fields):
        raise put.ProtocolError('invalid reader statistics scalar')
    if value['schema'] != 1 or value['outcome'] > 5:
        raise put.ProtocolError('unsupported reader statistics schema/outcome')
    for field in timing_fields:
        items = value.get(field)
        if not isinstance(items, list) or len(items) != 3 or not all(uint32(item) for item in items):
            raise put.ProtocolError('invalid reader timing tuple')
        if items[1] > items[0]:
            raise put.ProtocolError('reader timing maximum exceeds total')
    if (value['resumed'] > value['expected'] or value['accepted'] > value['expected']
            or value['socketBytes'] > value['expected'] - value['resumed']):
        raise put.ProtocolError('inconsistent reader statistics byte counts')
    return {field: value[field] for field in scalar_fields + timing_fields}


def report_reader_stats(host, identity, version, before, baseline, expected):
    # upload_once has already closed its socket, including on exception. A
    # failed optional observation must not replace the actual upload failure.
    try:
        after = put.wait_for_reader(host, 0, identity)
        if (after.get('version') != version or type(after.get('uptime')) is not int
                or after['uptime'] < before['uptime']):
            raise put.ProtocolError('reader restarted or changed firmware before statistics')
        record = read_reader_stats(host)
        if (record['attempt'] != baseline['attempt'] + 1 or record['outcome'] in (0, 1)
                or record['expected'] != expected):
            raise put.ProtocolError('statistics do not identify this completed stream attempt')
        print(json.dumps({'event': 'reader_metrics', 'record': record}, sort_keys=True), flush=True)
        return record
    except (put.ProtocolError, OSError, put.urllib.error.URLError) as error:
        print(json.dumps({'event': 'reader_metrics_unavailable', 'reason': str(error)}), flush=True)
        return None


def multipart_parts(name, boundary):
    # Names are generated here, not user-selected paths or firmware names.
    if not name.startswith('.pocket-bench-') or not name.endswith('.part') or any(
            c not in 'abcdefghijklmnopqrstuvwxyz0123456789.-' for c in name):
        raise ValueError('invalid benchmark staging name')
    prefix = (f'--{boundary}\r\nContent-Disposition: form-data; name="file"; '
              f'filename="{name}"\r\nContent-Type: application/octet-stream\r\n\r\n').encode()
    return prefix, f'\r\n--{boundary}--\r\n'.encode()


def http_upload(host, data, name, delay_ms):
    boundary = 'pocketbench' + uuid.uuid4().hex
    prefix, suffix = multipart_parts(name, boundary)
    connection = http.client.HTTPConnection(host, timeout=40)
    try:
        connection.connect()
        connection.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        started = time.monotonic()
        connection.putrequest('POST', '/upload?path=%2F')
        connection.putheader('Content-Type', f'multipart/form-data; boundary={boundary}')
        connection.putheader('Content-Length', str(len(prefix) + len(data) + len(suffix)))
        connection.putheader('Connection', 'close')
        connection.endheaders()
        connection.send(prefix)
        send_started = time.monotonic()
        for offset in range(0, len(data), 512):
            connection.send(data[offset:offset + 512])
            if delay_ms:
                time.sleep(delay_ms / 1000)
        send_seconds = time.monotonic() - send_started
        connection.send(suffix)
        reply_started = time.monotonic()
        response = connection.getresponse()
        body = response.read(4097)
        if response.status != 200 or len(body) > 4096:
            raise put.ProtocolError(f'HTTP upload rejected: {response.status}')
        return {'bytes': len(data), 'sent_bytes': len(data), 'resumed_bytes': 0,
                'elapsed_seconds': time.monotonic() - started,
                'send_and_pacing_seconds': send_seconds,
                'final_reply_seconds': time.monotonic() - reply_started}
    finally:
        connection.close()


class ReceiveBufferConnection(http.client.HTTPConnection):
    """Opt-in IPv4 experiment: configure only this socket before its SYN."""
    def __init__(self, host, receive_buffer, **kwargs):
        super().__init__(host, **kwargs)
        ipaddress.IPv4Address(self.host)
        if not 1024 <= receive_buffer <= 65536:
            raise ValueError('receive buffer must be 1024..65536')
        self.receive_buffer = receive_buffer

    def connect(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            sock.settimeout(self.timeout)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, self.receive_buffer)
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            if self.source_address:
                sock.bind(self.source_address)
            sock.connect((self.host, self.port))
            effective = sock.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF)
            print(json.dumps({'event': 'download_receive_buffer', 'requested': self.receive_buffer,
                              'effective': effective}, sort_keys=True), flush=True)
            self.sock = sock
        except BaseException:
            sock.close()
            raise


class ReceiveBufferHandler(put.urllib.request.HTTPHandler):
    def __init__(self, receive_buffer):
        super().__init__()
        self.receive_buffer = receive_buffer

    def http_open(self, request):
        return self.do_open(lambda host, **kwargs: ReceiveBufferConnection(
            host, self.receive_buffer, **kwargs), request)


def verify_download(host, target, data, receive_buffer=None):
    digest = hashlib.sha256()
    size = 0
    started = time.monotonic()
    reads = 0
    declared_bytes = None
    try:
        open_response = put.urllib.request.urlopen
        if receive_buffer is not None:
            # Keep urllib's request/response handling; no process-global opener
            # installation, proxy settings, or reader configuration changes.
            opener = put.urllib.request.build_opener(put.urllib.request.ProxyHandler({}),
                                                   ReceiveBufferHandler(receive_buffer))
            open_response = opener.open
        with open_response(
                f'http://{host}/download?' + urllib.parse.urlencode({'path': target}), timeout=40) as response:
            length = getattr(response, 'headers', {}).get('Content-Length')
            if isinstance(length, str) and length.isascii() and length.isdigit() and len(length) <= 19:
                declared_bytes = int(length)
            while True:
                # HTTPResponse.read(n) can consume a partial n-byte block and
                # then time out without returning it. read1 reports available
                # bytes after at most one underlying read, preserving evidence
                # of partial progress without changing requests or pacing.
                block = response.read1(4096)
                if not block:
                    break
                reads += 1
                size += len(block)
                if size > len(data):
                    raise put.ProtocolError('download exceeds expected size')
                digest.update(block)
        if size != len(data) or digest.digest() != hashlib.sha256(data).digest():
            raise put.ProtocolError('download size/SHA256 mismatch')
    except (OSError, http.client.HTTPException, put.ProtocolError) as error:
        print(json.dumps({'event': 'download_failed', 'received_bytes': size,
                          'expected_bytes': len(data), 'read_count': reads,
                          'declared_bytes': declared_bytes,
                          'received_prefix_matches': size <= len(data) and
                          digest.digest() == hashlib.sha256(data[:size]).digest(),
                          'elapsed_seconds': time.monotonic() - started,
                          'error_type': type(error).__name__}, sort_keys=True), flush=True)
        raise


def run_trial(host, transport, data, delay_ms, identity, version, reader_stats=False, receive_buffer=None):
    before = put.wait_for_reader(host, 0, identity)
    if before.get('version') != version:
        raise put.ProtocolError('firmware changed during comparison')
    baseline = None
    reader_record = None
    if reader_stats:
        if transport != 'stream' or type(before.get('uptime')) is not int or before['uptime'] < 0:
            raise put.ProtocolError('reader statistics require stream transport and valid uptime')
        baseline = read_reader_stats(host)
        if baseline['outcome'] == 1 or baseline['attempt'] == 0xFFFFFFFF:
            raise put.ProtocolError('reader statistics are active or attempt counter is saturated')
    token = uuid.uuid4().hex
    name = f'.pocket-bench-{token}.part'
    target = f'/pocket-bench-{token}.bin'
    published = False
    try:
        upload_started = time.monotonic()
        if transport == 'stream':
            try:
                metrics = put.upload_once(host, before, data, '/' + name, True, 0,
                                          flow_delay_ms=delay_ms,
                                          progress=lambda event: print(json.dumps(dict(event, event='stream_progress'),
                                                                                   sort_keys=True), flush=True))
            finally:
                upload_finished = time.monotonic()
                if reader_stats:
                    reader_record = report_reader_stats(host, identity, version, before, baseline, len(data))
        else:
            metrics = http_upload(host, data, name, delay_ms)
            upload_finished = time.monotonic()
        metrics['upload_including_setup_seconds'] = upload_finished - upload_started
        if reader_stats:
            metrics['reader_metrics'] = reader_record
        # Preserve upload measurements even if commit/readback later fails.
        print(json.dumps(dict(metrics, event='upload_result', transport=transport), sort_keys=True), flush=True)
        # Diagnostics are best effort, identity is not: a replacement reader
        # or changed firmware must never receive a publication command.
        current = put.wait_for_reader(host, 0, identity)
        if current.get('version') != version:
            raise put.ProtocolError('firmware changed before publication')
        print(json.dumps({'event': 'trial_phase', 'phase': 'commit'}), flush=True)
        commit_started = time.monotonic()
        put.commit(host, '/' + name, target, data)
        published = True
        metrics['commit_seconds'] = time.monotonic() - commit_started
        print(json.dumps({'event': 'trial_phase', 'phase': 'readback'}), flush=True)
        download_started = time.monotonic()
        verify_download(host, target, data, receive_buffer=receive_buffer)
        metrics['download_receive_buffer_requested'] = receive_buffer
        metrics['download_seconds'] = time.monotonic() - download_started
        print(json.dumps({'event': 'trial_phase', 'phase': 'status_confirmation'}), flush=True)
        after = put.wait_for_reader(host, 0, identity)
        if after.get('version') != version or after.get('uptime', 0) < before.get('uptime', 0):
            raise put.ProtocolError('reader restarted or changed firmware during trial')
        return dict(metrics, transport=transport, sha256=hashlib.sha256(data).hexdigest(),
                    version=version, delay_ms=delay_ms, downloaded_sha_verified=True,
                    free_heap_before=before.get('freeHeap'), free_heap_after=after.get('freeHeap'),
                    rssi_before=before.get('rssi'), rssi_after=after.get('rssi'))
    finally:
        if published:
            # Never send a delete to a replacement reader at the same IP.
            put.wait_for_reader(host, 0, identity)
            body = urllib.parse.urlencode({'path': target}).encode()
            request = put.urllib.request.Request(f'http://{host}/delete', data=body)
            with put.urllib.request.urlopen(request, timeout=15) as response:
                if response.status != 200:
                    raise put.ProtocolError('benchmark cleanup failed: ' + target)
            print('deleted benchmark file:', target, flush=True)
        else:
            print('publication not confirmed; inspect staging/target before cleanup:',
                  '/' + name, target, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True)
    parser.add_argument('--transport', choices=['http', 'stream'], required=True)
    parser.add_argument('--bytes', type=int, default=1024)
    parser.add_argument('--delay-ms', type=float, default=10)
    parser.add_argument('--repeat', type=int, default=1)
    parser.add_argument('--reader-stats', action='store_true',
                        help='opt-in developer stream counters; requires installed transfer-stats support')
    parser.add_argument('--download-receive-buffer', type=int,
                        help='IPv4-only, per-download SO_RCVBUF experiment (1024..65536); default unchanged')
    args = parser.parse_args()
    if not 1 <= args.bytes <= 8 * 1024 * 1024 or not 0 <= args.delay_ms <= 1000 or not 1 <= args.repeat <= 10:
        parser.error('bytes 1..8388608, delay 0..1000 ms, repeat 1..10 required')
    if args.reader_stats and args.transport != 'stream':
        parser.error('--reader-stats requires --transport stream')
    if args.download_receive_buffer is not None:
        if not 1024 <= args.download_receive_buffer <= 65536:
            parser.error('download receive buffer must be 1024..65536')
        try:
            ipaddress.IPv4Address(args.host)
        except ipaddress.AddressValueError:
            parser.error('download receive buffer experiment requires a literal IPv4 host')
    status = put.wait_for_reader(args.host, 0)
    if not status.get('deviceID') or not status.get('version'):
        parser.error('identified reader and firmware version required')
    data = benchmark_payload(args.bytes)
    print(json.dumps({'event': 'benchmark_payload', 'pattern': PAYLOAD_PATTERN,
                      'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}, sort_keys=True), flush=True)
    for _ in range(args.repeat):
        result = run_trial(args.host, args.transport, data, args.delay_ms,
                           status['deviceID'], status['version'], args.reader_stats, args.download_receive_buffer)
        result['payload_pattern'] = PAYLOAD_PATTERN
        print(json.dumps(result, sort_keys=True), flush=True)


if __name__ == '__main__':
    main()
