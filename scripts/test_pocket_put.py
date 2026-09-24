"""Host-only recovery/verification tests: python3 -m unittest discover -s scripts -p 'test_pocket_put.py'."""
import argparse
import io
import json
import unittest
from unittest.mock import patch
import zlib

import pocket_put as put


class Socket:
    def __init__(self, replies):
        self.replies = io.BytesIO(replies)
        self.sent = []

    def __enter__(self):
        return self

    def __exit__(self, *args):
        pass

    def setsockopt(self, *args):
        pass

    def sendall(self, data):
        self.sent.append(data)

    def recv(self, size):
        return self.replies.read(size)


class RecoveryTests(unittest.TestCase):
    def setUp(self):
        self.args = argparse.Namespace(host="reader", attempts=3, wait_seconds=0,
                                       resume=True, legacy_delay_ms=0, legacy_chunk_bytes=4096, flow_delay_ms=0)
        self.status = {"deviceID": "test-reader", "uploadStreamPort": 82,
                       "uploadStreamResume": True, "uploadStreamWindow": 4096}

    def test_retry_preserves_identity_and_staging(self):
        with patch.object(put, "wait_for_reader", return_value=self.status) as status, \
                patch.object(put, "upload_once", side_effect=[ConnectionError(), None]) as upload, \
                patch.object(put.time, "sleep"):
            put.upload_with_recovery(self.args, b"data", "/.pocket-test.part")
        self.assertEqual(upload.call_count, 2)
        self.assertEqual(upload.call_args_list[0], upload.call_args_list[1])
        self.assertEqual(status.call_args_list[1].args[2], "test-reader")

    def test_retry_is_bounded(self):
        with patch.object(put, "wait_for_reader", return_value=self.status), \
                patch.object(put, "upload_once", side_effect=ConnectionError()) as upload, \
                patch.object(put.time, "sleep"):
            with self.assertRaises(ConnectionError):
                put.upload_with_recovery(self.args, b"data", "/stage")
        self.assertEqual(upload.call_count, 3)

    def test_no_automatic_retry_without_identity(self):
        self.status.pop("deviceID")
        with patch.object(put, "wait_for_reader", return_value=self.status), \
                patch.object(put, "upload_once", side_effect=ConnectionError()) as upload:
            with self.assertRaises(ConnectionError):
                put.upload_with_recovery(self.args, b"data", "/stage")
        self.assertEqual(upload.call_count, 1)

    def test_protocol_failure_is_terminal(self):
        with patch.object(put, "wait_for_reader", return_value=self.status), \
                patch.object(put, "upload_once", side_effect=put.ProtocolError()) as upload:
            with self.assertRaises(put.ProtocolError):
                put.upload_with_recovery(self.args, b"data", "/stage")
        self.assertEqual(upload.call_count, 1)

    def test_status_identity_mismatch(self):
        response = io.BytesIO(json.dumps(self.status).encode())
        with patch.object(put.urllib.request, "urlopen", return_value=response):
            with self.assertRaisesRegex(put.ProtocolError, "identity changed"):
                put.wait_for_reader("reader", 0, "another-reader")

    def test_complete_resume_verifies_final_crc_without_payload(self):
        data = b"existing complete file"
        sock = Socket(f"RESUME {len(data)}\nOK {len(data)} {zlib.crc32(data):08X}\n".encode())
        with patch.object(put.socket, "create_connection", return_value=sock):
            put.upload_once("reader", self.status, data, "/stage", True, 0)
        self.assertEqual(len(sock.sent), 1)

    def test_flow_control_and_short_tail(self):
        data = b"x" * 4101
        sock = Socket(f"RESUME 0\nACK 4096\nOK 4101 {zlib.crc32(data):08X}\n".encode())
        with patch.object(put.socket, "create_connection", return_value=sock):
            metrics = put.upload_once("reader", self.status, data, "/stage", True, 0)
        self.assertEqual([len(part) for part in sock.sent[1:]], [4096, 5])
        self.assertEqual(metrics['credit_count'], 1)
        self.assertEqual(metrics['sent_bytes'], 4101)
        self.assertEqual(metrics['resumed_bytes'], 0)
        self.assertGreaterEqual(metrics['elapsed_seconds'], metrics['credit_wait_seconds'])

    def test_bad_ack_is_terminal(self):
        sock = Socket(b"RESUME 0\nACK 4097\n")
        with patch.object(put.socket, "create_connection", return_value=sock):
            with self.assertRaises(put.ProtocolError):
                put.upload_once("reader", self.status, b"x" * 5000, "/stage", True, 0)
        self.assertEqual(len(sock.sent), 2)

    def test_progress_keeps_submission_separate_from_verified_sd_prefix(self):
        data = b'x' * 4101
        sock = Socket(f'RESUME 0\nACK 4096\nOK 4101 {zlib.crc32(data):08X}\n'.encode())
        events = []
        with patch.object(put.socket, 'create_connection', return_value=sock):
            put.upload_once('reader', self.status, data, '/stage', True, 0, progress=events.append)
        self.assertEqual([e['phase'] for e in events],
                         ['ready', 'submitted', 'acknowledged', 'submitted', 'verified'])
        self.assertEqual([e['acknowledged_bytes'] for e in events], [0, 0, 4096, 4096, 4101])
        self.assertEqual([e['submitted_bytes'] for e in events], [0, 4096, 4096, 4101, 4101])

    def test_timeout_progress_does_not_claim_unacknowledged_window(self):
        sock = Socket(b'RESUME 0\nACK 4096\nERROR Upload timed out\n')
        events = []
        with patch.object(put.socket, 'create_connection', return_value=sock):
            with self.assertRaises(ConnectionError):
                put.upload_once('reader', self.status, b'x' * 9000, '/stage', True, 0, progress=events.append)
        self.assertEqual(events[-1]['submitted_bytes'], 8192)
        self.assertEqual(events[-1]['acknowledged_bytes'], 4096)
        self.assertNotIn('verified', [e['phase'] for e in events])

    def test_corrupt_final_reply_never_reports_verified(self):
        events = []
        with patch.object(put.socket, 'create_connection', return_value=Socket(b'RESUME 0\nOK 4 00000000\n')):
            with self.assertRaises(put.ProtocolError):
                put.upload_once('reader', self.status, b'data', '/stage', True, 0, progress=events.append)
        self.assertEqual(events[-1]['acknowledged_bytes'], 0)
        self.assertNotIn('verified', [e['phase'] for e in events])

    def test_flow_pacing_preserves_credit_boundary(self):
        data = b"x" * 4101
        sock = Socket(f"RESUME 0\nACK 4096\nOK 4101 {zlib.crc32(data):08X}\n".encode())
        with patch.object(put.socket, "create_connection", return_value=sock), patch.object(put.time, "sleep"):
            put.upload_once("reader", self.status, data, "/stage", True, 0, 4096, 10)
        self.assertEqual([len(part) for part in sock.sent[1:]], [512] * 8 + [5])

    def test_paced_legacy_small_writes(self):
        data = b"x" * 1025
        status = dict(self.status)
        status.pop("uploadStreamWindow")
        sock = Socket(f"RESUME 0\nOK 1025 {zlib.crc32(data):08X}\n".encode())
        with patch.object(put.socket, "create_connection", return_value=sock), patch.object(put.time, "sleep"):
            put.upload_once("reader", status, data, "/stage", True, 20, 512)
        self.assertEqual([len(part) for part in sock.sent[1:]], [512, 512, 1])

    def test_partial_reply_is_transport_failure(self):
        with self.assertRaises(ConnectionError):
            put.read_line(Socket(b"OK 10"))

    def test_error_classification(self):
        with self.assertRaises(ConnectionError):
            put.read_line(Socket(b"ERROR Upload timed out\n"))
        with self.assertRaises(put.ProtocolError):
            put.read_line(Socket(b"ERROR SD write failed\n"))

    def test_ambiguous_commit_never_retried(self):
        with patch.object(put.urllib.request, "urlopen", side_effect=TimeoutError()) as request:
            with self.assertRaisesRegex(put.ProtocolError, "outcome unknown"):
                put.commit("reader", "/stage", "/target", b"data")
        self.assertEqual(request.call_count, 1)

    def test_lost_flash_reply_polls_identity_and_version_without_reflash(self):
        self.status["version"] = "old-build"
        installed = dict(self.status, version="new-build")
        with patch.object(put.urllib.request, "urlopen", side_effect=TimeoutError()) as request, \
                patch.object(put, "wait_for_reader", side_effect=[self.status, installed]) as status, \
                patch.object(put.time, "sleep"):
            put.flash_developer_image("reader", self.status, "new-build")
        self.assertEqual(request.call_count, 1)
        self.assertEqual(status.call_count, 2)
        self.assertEqual(status.call_args.args[2], "test-reader")

    def test_same_version_cannot_prove_installation(self):
        self.status["version"] = "same-build"
        with patch.object(put.urllib.request, "urlopen") as request:
            with self.assertRaises(put.ProtocolError):
                put.flash_developer_image("reader", self.status, "same-build")
        request.assert_not_called()

    def test_flash_timeout_does_not_repeat_post(self):
        with patch.object(put.urllib.request, "urlopen", return_value=io.BytesIO(b"flashing")) as request:
            with self.assertRaisesRegex(put.ProtocolError, "not verified"):
                put.flash_developer_image("reader", self.status, "new-build", timeout=0)
        self.assertEqual(request.call_count, 1)

    def test_embedded_developer_version(self):
        self.assertEqual(put.developer_version(
            b"CrossPoint version: 1.6.6-dev-sta-recovery\n\0PocketDaily-ESP32-1.6.6-dev-sta-recovery\0"),
            "1.6.6-dev-sta-recovery")
        self.assertEqual(put.developer_version(b"header\0Starting CrossPoint version 1.6.6-dev-main-abcd-w1234\0"),
                         "1.6.6-dev-main-abcd-w1234")
        self.assertEqual(put.developer_version(b"Starting CrossPoint version 1.6.6-dev-main-abcd-w1234\n\0"),
                         "1.6.6-dev-main-abcd-w1234")
        for image in (b"unknown", b"Starting CrossPoint version 1.6.6\0",
                      b"Starting CrossPoint version 1-dev-one\0Starting CrossPoint version 1-dev-two\0"):
            with self.assertRaises(put.ProtocolError):
                put.developer_version(image)


if __name__ == "__main__":
    unittest.main()
