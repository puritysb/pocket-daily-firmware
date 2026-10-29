import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import dev_trial_transfer as ft

class Socket:
    def __init__(self, pieces): self.pieces = list(pieces)
    def recv(self, _): return self.pieces.pop(0) if self.pieces else b''
    def setsockopt(self, *_): pass

def response(data, declared=None, extra=b'', http='HTTP/1.1'):
    size = len(data) if declared is None else declared
    return [f'{http} 200 OK\r\nContent-Length: {size}\r\nConnection: close\r\n\r\n'.encode(), data, extra]

class TrialTransfer(unittest.TestCase):
    def test_exact_whole_bytes_and_empty_body(self):
        for data in (b'', bytes((i*37+i//31)%251 for i in range(1297))):
            result = ft.receive(Socket(response(data)), data)
            self.assertEqual(result['bytes'], len(data)); self.assertTrue(result['complete'])

    def test_short_length_wrong_bytes_and_extra_data_fail(self):
        expected = b'uniform values cannot cover transport offsets'
        for parts in (response(expected[:-1], declared=len(expected)), response(expected, declared=1),
                      response(expected[::-1]), response(expected, extra=b'x')):
            with self.assertRaises(ValueError): ft.receive(Socket(parts), expected)

    def test_duplicate_length_chunking_and_wrong_http_version_fail(self):
        for header in (b'HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 1\r\nConnection: close\r\n\r\n',
                       b'HTTP/1.1 200 OK\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n'):
            with self.assertRaises(ValueError): ft.receive(Socket([header,b'x']),b'x')
        with self.assertRaises(ValueError): ft.receive(Socket(response(b'x',http='HTTP/1.0')),b'x')
        self.assertTrue(ft.receive(Socket(response(b'x',http='HTTP/1.0')),b'x',http='HTTP/1.0')['complete'])

    def test_missing_or_oversized_headers_fail(self):
        for parts in ([b''], [b'x'*8192]):
            with self.assertRaises(ValueError): ft.receive(Socket(parts), b'x')

    def test_expected_interruptions_require_a_correct_partial_prefix(self):
        data = bytes(i%251 for i in range(15000))
        result = ft.receive(Socket(response(data[:9000], declared=len(data))), data, mode='rst')
        self.assertFalse(result['complete'])
        with patch.object(ft.time,'sleep'):
            self.assertFalse(ft.receive(Socket(response(data[:123], declared=len(data))), data, mode='stall')['complete'])
            with self.assertRaises(ValueError): ft.receive(Socket(response(data)), data, mode='stall')

    def test_total_deadline_is_enforced_even_with_progress(self):
        with patch.object(ft.time,'monotonic',side_effect=[0,601]):
            with self.assertRaises(ValueError): ft.receive(Socket(response(b'abc')),b'abc')

    def test_fixtures_are_exact_size_valid_epub_and_path_bound(self):
        data = ft.epub(1024*1024)
        self.assertEqual(len(data),1024*1024)
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            self.assertEqual(archive.namelist()[0],'mimetype')
            self.assertEqual(archive.read('mimetype'),b'application/epub+zip')
            self.assertIn(b'Checkpoint 000001',archive.read('book.xhtml'))
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            rows = ft.freeze(root, 731)
            plan = {'run':731,'fixtures':rows}
            self.assertEqual(len(ft.checked_fixtures(root,plan)),4)
            rows[0]['path']='/Books/original.epub'
            with self.assertRaises(ValueError): ft.checked_fixtures(root,plan)
