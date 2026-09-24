"""Source-boundary guard, not a pixel/rendering equivalence test."""
from pathlib import Path
import re
import unittest


class ThemeMetricBindingTests(unittest.TestCase):
    def test_renderers_do_not_bypass_active_metrics(self):
        root = Path(__file__).resolve().parents[1] / 'src/components/themes'
        renderers = list(root.rglob('*.cpp'))
        self.assertGreaterEqual(len(renderers), 4)
        for source in renderers:
            with self.subTest(source=source.name):
                text = source.read_text()
                self.assertIsNone(re.search(r'\b\w+Metrics\s*::\s*values\b', text),
                                  'Native constants belong in theme selection, not runtime drawing')


if __name__ == '__main__':
    unittest.main()
