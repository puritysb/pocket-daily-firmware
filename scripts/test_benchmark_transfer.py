import io
import json
import http.server
import threading
import unittest
from unittest.mock import Mock, patch

import benchmark_transfer as bench


class BenchmarkTests(unittest.TestCase):
    def test_payload_has_distinct_blocks_and_stable_prefix(self):
        data = bench.benchmark_payload(16384)
        self.assertEqual(len(data), 16384)
        self.assertEqual(data, bench.benchmark_payload(16384))
        self.assertEqual(data[:1024], bench.benchmark_payload(1024))
        for block_size in [256, 512, 1024, 4096]:
            blocks = [data[offset:offset + block_size] for offset in range(0, len(data), block_size)]
            self.assertEqual(len(set(blocks)), len(blocks))
        for invalid in [0, -1, 8388609, True, 1.5]:
            with self.subTest(size=invalid), self.assertRaises(ValueError):
                bench.benchmark_payload(invalid)

    def test_readback_rejects_equal_length_duplicate_and_reordered_blocks(self):
        data = bench.benchmark_payload(16384)
        duplicated = data[:4096] + data[:4096] + data[8192:]
        reordered = data[4096:8192] + data[:4096] + data[8192:]
        for corrupted in [duplicated, reordered]:
            self.assertEqual(len(corrupted), len(data))
            output = io.StringIO()
            with patch.object(bench.put.urllib.request, 'urlopen', return_value=io.BytesIO(corrupted)), \
                    patch('sys.stdout', output):
                with self.assertRaises(bench.put.ProtocolError):
                    bench.verify_download('reader', '/test.bin', data)
            event = json.loads(output.getvalue())
            self.assertEqual(event['received_bytes'], len(data))
            self.assertFalse(event['received_prefix_matches'])

    def test_receive_buffer_handler_downloads_and_verifies_over_loopback(self):
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                self.send_response(200)
                self.send_header('Content-Length', '5')
                self.end_headers()
                self.wfile.write(b'valid')

            def log_message(self, *_args):
                pass

        # HTTPServer's constructor otherwise performs a reverse-DNS lookup,
        # unrelated to this numeric-loopback transport test.
        with patch.object(bench.socket, 'getfqdn', return_value='localhost'):
            server = http.server.HTTPServer(('127.0.0.1', 0), Handler)
        worker = threading.Thread(target=lambda: server.serve_forever(poll_interval=0.01), daemon=True)
        worker.start()
        output = io.StringIO()
        try:
            with patch('sys.stdout', output):
                bench.verify_download(f'127.0.0.1:{server.server_port}', '/test.bin', b'valid', receive_buffer=4096)
            event = json.loads(output.getvalue())
            self.assertEqual(event['event'], 'download_receive_buffer')
            self.assertEqual(event['requested'], 4096)
            self.assertGreater(event['effective'], 0)
        finally:
            server.shutdown()
            server.server_close()
            worker.join(timeout=1)
        self.assertFalse(worker.is_alive())

    def test_receive_buffer_is_set_before_connect_and_socket_is_owned(self):
        sock = Mock()
        sock.getsockopt.return_value = 4096
        with patch.object(bench.socket, 'socket', return_value=sock):
            connection = bench.ReceiveBufferConnection('127.0.0.1', 4096, timeout=40)
            connection.connect()
            self.assertIs(connection.sock, sock)
            calls = [call[0] for call in sock.mock_calls]
            self.assertLess(calls.index('setsockopt'), calls.index('connect'))
            sock.setsockopt.assert_any_call(bench.socket.SOL_SOCKET, bench.socket.SO_RCVBUF, 4096)
            connection.close()
            sock.close.assert_called_once()

    def test_receive_buffer_connect_failure_closes_socket(self):
        sock = Mock()
        sock.connect.side_effect = TimeoutError('connect')
        with patch.object(bench.socket, 'socket', return_value=sock):
            connection = bench.ReceiveBufferConnection('127.0.0.1', 4096, timeout=40)
            with self.assertRaises(TimeoutError):
                connection.connect()
            sock.close.assert_called_once()
            self.assertIsNone(connection.sock)

    def test_receive_buffer_rejects_invalid_parameters_without_socket(self):
        with patch.object(bench.socket, 'socket') as create:
            for host, size in [('reader', 4096), ('::1', 4096), ('127.0.0.1', 0), ('127.0.0.1', 65537)]:
                with self.subTest(host=host, size=size), self.assertRaises(ValueError):
                    bench.ReceiveBufferConnection(host, size)
            create.assert_not_called()

    def stats(self, **changes):
        record = dict(schema=1, attempt=1, outcome=2, expected=4, resumed=0,
                      socketBytes=4, accepted=4, elapsedMs=10, maxServiceGapMs=2,
                      maxReceiveGapMs=3, minHeap=10000, minBlock=6000,
                      sdWriteUs=[100, 100, 1], sdCloseUs=[50, 50, 1], replyWriteUs=[20, 10, 2])
        return dict(record, **changes)

    def read_stats(self, value):
        with patch.object(bench.put.urllib.request, 'urlopen', return_value=io.BytesIO(json.dumps(value).encode())):
            return bench.read_reader_stats('reader')

    def test_reader_stats_are_whitelisted(self):
        record = self.stats(deviceID='must-not-be-reported', arbitrary='private')
        self.assertEqual(self.read_stats(record), self.stats())

    def test_reader_stats_reject_bad_shapes_counts_and_ranges(self):
        for value in [[], None, {}, self.stats(schema=True), self.stats(schema=2), self.stats(outcome=6),
                      self.stats(attempt=-1), self.stats(attempt=2**32), self.stats(elapsedMs=1.5),
                      self.stats(sdWriteUs=[1, 2, 1]), self.stats(sdWriteUs=[1, 1]),
                      self.stats(sdWriteUs=[1, 1, False]), self.stats(expected=3), self.stats(resumed=5)]:
            with self.subTest(value=value), self.assertRaises(bench.put.ProtocolError):
                self.read_stats(value)

    def test_reader_stats_body_is_bounded_and_json_required(self):
        for body in [b'x' * 2049, b'not json', b'\xff']:
            with patch.object(bench.put.urllib.request, 'urlopen', return_value=io.BytesIO(body)):
                with self.assertRaises(bench.put.ProtocolError):
                    bench.read_reader_stats('reader')

    def test_reader_stats_must_match_the_new_terminal_attempt(self):
        baseline = self.stats(attempt=8)
        with patch.object(bench.put, 'wait_for_reader', return_value={'version': 'v1', 'uptime': 101}):
            for record in [self.stats(attempt=8), self.stats(attempt=10),
                           self.stats(attempt=9, outcome=1), self.stats(attempt=9, expected=8)]:
                with patch.object(bench, 'read_reader_stats', return_value=record):
                    self.assertIsNone(bench.report_reader_stats('reader', 'id', 'v1', {'uptime': 100}, baseline, 4))
            with patch.object(bench, 'read_reader_stats', return_value=self.stats(attempt=9)):
                self.assertEqual(bench.report_reader_stats('reader', 'id', 'v1', {'uptime': 100}, baseline, 4),
                                 self.stats(attempt=9))

    def test_reader_stats_do_not_accept_changed_boot_or_bad_uptime(self):
        for status in [{'version': 'v2', 'uptime': 101}, {'version': 'v1', 'uptime': 5},
                       {'version': 'v1', 'uptime': None}, {'version': 'v1', 'uptime': True}]:
            with patch.object(bench.put, 'wait_for_reader', return_value=status), \
                    patch.object(bench, 'read_reader_stats') as fetch:
                self.assertIsNone(bench.report_reader_stats('reader', 'id', 'v1', {'uptime': 100}, self.stats(), 4))
                fetch.assert_not_called()

    def test_failed_stats_observation_preserves_original_upload_failure(self):
        original = ConnectionError('original upload failure')
        with patch.object(bench.put, 'wait_for_reader', side_effect=[{'version': 'v1', 'uptime': 100}, TimeoutError()]), \
                patch.object(bench, 'read_reader_stats', return_value=self.stats(attempt=0, outcome=0)), \
                patch.object(bench.put, 'upload_once', side_effect=original), \
                patch.object(bench.put, 'commit') as commit:
            with self.assertRaises(ConnectionError) as caught:
                bench.run_trial('reader', 'stream', b'data', 10, 'id', 'v1', reader_stats=True)
        self.assertIs(caught.exception, original)
        commit.assert_not_called()

    def test_active_or_saturated_stats_stop_before_upload(self):
        for record in [self.stats(outcome=1), self.stats(attempt=0xFFFFFFFF)]:
            with patch.object(bench.put, 'wait_for_reader', return_value={'version': 'v1', 'uptime': 100}), \
                    patch.object(bench, 'read_reader_stats', return_value=record), \
                    patch.object(bench.put, 'upload_once') as upload:
                with self.assertRaises(bench.put.ProtocolError):
                    bench.run_trial('reader', 'stream', b'data', 10, 'id', 'v1', reader_stats=True)
                upload.assert_not_called()

    def test_multipart_delimiters(self):
        prefix, suffix = bench.multipart_parts('.pocket-bench-abcd.part', 'boundary')
        self.assertTrue(prefix.startswith(b'--boundary\r\n'))
        self.assertTrue(prefix.endswith(b'\r\n\r\n'))
        self.assertEqual(suffix, b'\r\n--boundary--\r\n')

    def test_only_generated_staging_paths(self):
        for name in ['update.bin', '../.pocket-bench-x.part', '.pocket-bench-".part',
                     '.pocket-bench-x.part\r\n', '/.pocket-bench-x.part']:
            with self.assertRaises(ValueError):
                bench.multipart_parts(name, 'boundary')

    def test_readback_exact_hash(self):
        with patch.object(bench.put.urllib.request, 'urlopen', return_value=io.BytesIO(b'valid')):
            bench.verify_download('reader', '/test.bin', b'valid')

    def test_readback_timeout_preserves_sub_block_progress(self):
        class PartialResponse(io.BytesIO):
            headers = {'Content-Length': '5'}
            def read1(self, size):
                if self.tell():
                    raise TimeoutError('timed out')
                return super().read1(min(size, 3))

        output = io.StringIO()
        with patch.object(bench.put.urllib.request, 'urlopen', return_value=PartialResponse(b'valid')), \
                patch('sys.stdout', output):
            with self.assertRaises(TimeoutError):
                bench.verify_download('reader', '/private-name.bin', b'valid')
        event = json.loads(output.getvalue())
        self.assertEqual(event['event'], 'download_failed')
        self.assertEqual(event['received_bytes'], 3)
        self.assertEqual(event['expected_bytes'], 5)
        self.assertEqual(event['read_count'], 1)
        self.assertEqual(event['declared_bytes'], 5)
        self.assertTrue(event['received_prefix_matches'])
        self.assertEqual(event['error_type'], 'TimeoutError')
        self.assertNotIn('private-name', output.getvalue())
        self.assertNotIn('val', output.getvalue())

    def test_readback_fragmented_delivery_keeps_exact_hash_check(self):
        class FragmentedResponse(io.BytesIO):
            def read1(self, size):
                return super().read1(min(size, 1))

        with patch.object(bench.put.urllib.request, 'urlopen', return_value=FragmentedResponse(b'valid')):
            bench.verify_download('reader', '/test.bin', b'valid')

    def test_readback_corruption_short_and_long(self):
        for payload in [b'other', b'val', b'valid-extra']:
            with patch.object(bench.put.urllib.request, 'urlopen', return_value=io.BytesIO(payload)):
                with self.assertRaises(bench.put.ProtocolError):
                    bench.verify_download('reader', '/test.bin', b'valid')

    def test_changed_firmware_stops_before_writes(self):
        with patch.object(bench.put, 'wait_for_reader', return_value={'version': 'other'}), \
                patch.object(bench, 'http_upload') as upload:
            with self.assertRaises(bench.put.ProtocolError):
                bench.run_trial('reader', 'http', b'data', 10, 'id', 'expected')
            upload.assert_not_called()

    def test_identity_or_firmware_change_after_upload_prevents_commit(self):
        for next_status in [bench.put.ProtocolError('identity changed'), {'version': 'v2'}]:
            with patch.object(bench.put, 'wait_for_reader', side_effect=[{'version': 'v1'}, next_status]), \
                    patch.object(bench.put, 'upload_once', return_value={}), \
                    patch.object(bench.put, 'commit') as commit:
                with self.assertRaises(bench.put.ProtocolError):
                    bench.run_trial('reader', 'stream', b'data', 10, 'id', 'v1')
                commit.assert_not_called()

    def test_verified_publication_is_cleaned_up(self):
        status = {'version': 'v1', 'uptime': 100}
        response = io.BytesIO(b'ok')
        response.status = 200
        with patch.object(bench.put, 'wait_for_reader', return_value=status), \
                patch.object(bench.put, 'upload_once', return_value={'bytes': 4}), \
                patch.object(bench.put, 'commit'), \
                patch.object(bench, 'verify_download'), \
                patch.object(bench.put.urllib.request, 'urlopen', return_value=response) as request:
            result = bench.run_trial('reader', 'stream', b'data', 10, 'id', 'v1')
        self.assertTrue(result['downloaded_sha_verified'])
        self.assertIn(b'path=%2Fpocket-bench-', request.call_args.args[0].data)

    def test_ambiguous_commit_never_deletes(self):
        with patch.object(bench.put, 'wait_for_reader', return_value={'version': 'v1'}), \
                patch.object(bench.put, 'upload_once', return_value={}), \
                patch.object(bench.put, 'commit', side_effect=bench.put.ProtocolError('unknown')), \
                patch.object(bench.put.urllib.request, 'urlopen') as request:
            with self.assertRaises(bench.put.ProtocolError):
                bench.run_trial('reader', 'stream', b'data', 10, 'id', 'v1')
        request.assert_not_called()

    def test_replaced_reader_never_receives_cleanup(self):
        with patch.object(bench.put, 'wait_for_reader', side_effect=[
                {'version': 'v1'}, bench.put.ProtocolError('identity changed'),
                bench.put.ProtocolError('identity changed')]), \
                patch.object(bench.put, 'upload_once', return_value={}), \
                patch.object(bench.put, 'commit'), \
                patch.object(bench, 'verify_download'), \
                patch.object(bench.put.urllib.request, 'urlopen') as request:
            with self.assertRaises(bench.put.ProtocolError):
                bench.run_trial('reader', 'stream', b'data', 10, 'id', 'v1')
        request.assert_not_called()


if __name__ == '__main__':
    unittest.main()
