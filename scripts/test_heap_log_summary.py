import unittest
from pathlib import Path
import importlib.util

SPEC = importlib.util.spec_from_file_location("heap_log_summary", Path(__file__).with_name("heap_log_summary.py"))
heap = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(heap)


def mem(free, largest):
    return f"[1] [INF] [MEM] Free: {free} bytes, Total: 300000 bytes, Min Free: 20000 bytes, MaxAlloc: {largest} bytes"


class HeapLogSummaryTest(unittest.TestCase):
    def test_takes_first_mem_line_after_each_run_of_page_renders(self):
        lines = [
            mem(90000, 80000),  # before any page: not a sample
            "[2] [DBG] [ERS] Rendered page in 31ms",
            "[3] [DBG] [ERS] Rendered page in 30ms",
            mem(54980, 30708),  # sample 1
            mem(54980, 30000),  # idle repeat: not a sample
            "[4] [DBG] [ERS] Rendered page in 29ms",
            "[5] [INF] [MEM] PSRAM: Free: 1 bytes, Total: 2 bytes, Min Free: 1 bytes, MaxAlloc: 1 bytes",
            mem(54976, 17396),  # sample 2
        ]
        self.assertEqual(heap.samples(lines), [(54980, 30708), (54976, 17396)])

    def test_summary_and_garbage_lines(self):
        self.assertEqual(heap.samples(["\x00\xff garbage", "Rendered page in", "MaxAlloc: 5 bytes"]), [])
        self.assertEqual(heap.summarise([3, 1, 2]), {"n": 3, "median": 2, "min": 1, "max": 3})


if __name__ == "__main__":
    unittest.main()
