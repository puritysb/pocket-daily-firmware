import io
import unittest
from unittest.mock import patch

import dev_ble_cycle as cycle


class CycleChecks(unittest.TestCase):
    def setUp(self):
        self.result = dict(state=3, frameReleased=True, frameBytes=52272,
                           beforeFree=85123, afterFree=137411, statsValid=True,
                           gate="open", opened=1, minFree=61234, minBlock=17280,
                           connections=1, lists=2)

    def test_complete_exchange(self):
        cycle.check_result(self.result, require_exchange=True)

    def test_each_missing_success_condition_fails(self):
        for key, value in (("state", 4), ("frameReleased", False), ("frameBytes", 0),
                           ("afterFree", 85123), ("statsValid", False), ("gate", "low-memory"),
                           ("opened", 0), ("minFree", 50 * 1024), ("minBlock", 8191),
                           ("connections", 0), ("lists", 0)):
            with self.subTest(key=key), self.assertRaises(cycle.pocket_put.ProtocolError):
                cycle.check_result(dict(self.result, **{key: value}), require_exchange=True)

    def test_advertising_is_not_exchange_acceptance(self):
        self.result.update(connections=0, lists=0)
        cycle.check_result(self.result)
        with self.assertRaises(cycle.pocket_put.ProtocolError):
            cycle.check_result(self.result, require_exchange=True)

    def test_identity_and_exact_version_are_required(self):
        cycle.verify_identity({"deviceID": "test", "version": "dev-a"}, "test", "dev-a")
        for status in ({"deviceID": "other", "version": "dev-a"},
                       {"deviceID": "test", "version": "dev-b"}, []):
            with self.assertRaises(cycle.pocket_put.ProtocolError):
                cycle.verify_identity(status, "test", "dev-a")

    def test_app_wake_requires_actual_sleep_and_authenticated_handoff(self):
        self.result.update(returnMode=2, fromWifi=True, close="app-wifi", lightSleeps=15, lightSleepMs=231)
        cycle.check_result(self.result, standby=True)
        for key, value in (("fromWifi", False), ("returnMode", 1), ("close", "time-up"), ("lightSleeps", 0), ("lightSleepMs", 0)):
            with self.subTest(key=key), self.assertRaises(cycle.pocket_put.ProtocolError):
                cycle.check_result(dict(self.result, **{key: value}), standby=True)

    def test_software_return_cannot_pass_as_timer_wake(self):
        with self.assertRaises(cycle.pocket_put.ProtocolError):
            cycle.check_result(self.result, timer_sleep=True)
        self.result.update(returnMode=1, timerArmed=True, timerWake=True)
        cycle.check_result(self.result, timer_sleep=True)
        for key, value in (("returnMode", 0), ("timerArmed", False), ("timerWake", False)):
            with self.subTest(key=key), self.assertRaises(cycle.pocket_put.ProtocolError):
                cycle.check_result(dict(self.result, **{key: value}), timer_sleep=True)

    def test_lost_post_reply_never_retries(self):
        with patch.object(cycle.urllib.request, "urlopen", side_effect=TimeoutError()) as request:
            cycle.start_cycle("reader", 123)
        self.assertEqual(request.call_count, 1)

    def test_http_rejection_is_terminal(self):
        error = cycle.urllib.error.HTTPError("url", 409, "busy", {}, io.BytesIO())
        with patch.object(cycle.urllib.request, "urlopen", side_effect=error) as request:
            with self.assertRaises(cycle.pocket_put.ProtocolError):
                cycle.start_cycle("reader", 123)
        self.assertEqual(request.call_count, 1)

    def test_old_completed_record_does_not_satisfy_new_run(self):
        status = {"deviceID": "test", "version": "dev-a"}
        with patch.object(cycle, "get_json", side_effect=[status, {"run": 12, "state": 3},
                                                           status, {"run": 13, "state": 3}]), \
                patch.object(cycle.time, "sleep"):
            host, actual, record = cycle.wait_cycle("reader", "test", "dev-a", 13, 30)
        self.assertEqual(record["run"], 13)
        self.assertEqual(host, "reader")
        self.assertEqual(actual, status)

    def test_discovery_scope_is_bounded(self):
        for subnet in ("192.168.0.0/16", "8.8.8.0/24", "::/0"):
            with self.assertRaises(ValueError):
                cycle.discover(subnet, "test")


if __name__ == "__main__":
    unittest.main()
