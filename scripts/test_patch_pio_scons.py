import json
from pathlib import Path
import tempfile
import unittest
from patch_pio_scons import patch, VERSION, URL, OLD_URL


class SConsMetadataTests(unittest.TestCase):
    def test_aligns_both_fields_and_preserves_other_packages(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "platform.json"
            data = {"packages": {"tool-scons": {"version": OLD_URL, "package-version": "4.40801.0", "optional": True}, "other": {"version": "keep"}}}
            p.write_text(json.dumps(data))
            self.assertTrue(patch(p))
            result = json.loads(p.read_text())
            self.assertEqual(result["packages"]["other"], data["packages"]["other"])
            self.assertEqual(result["packages"]["tool-scons"], {"version": URL, "package-version": VERSION, "optional": True})
            before = p.read_bytes()
            self.assertFalse(patch(p))
            self.assertEqual(before, p.read_bytes())

    def test_unknown_version_is_not_modified(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "platform.json"
            p.write_text(json.dumps({"packages": {"tool-scons": {"version": URL, "package-version": "5.0"}}}))
            before = p.read_bytes()
            with self.assertRaises(RuntimeError):
                patch(p)
            self.assertEqual(before, p.read_bytes())
