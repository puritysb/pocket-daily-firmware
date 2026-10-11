import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from patch_storage_sdk import apply_storage_patch


class StorageSDKPatchTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.project = Path(self.temporary.name)
        root = Path(__file__).resolve().parents[1]
        relative = Path("freeink-sdk/libs/hardware/SDCardManager/include/SDCardManager.h")
        self.header = self.project / relative
        self.header.parent.mkdir(parents=True)
        self.original = subprocess.check_output(
            ["git", "-C", str(root / "freeink-sdk"), "show", "HEAD:libs/hardware/SDCardManager/include/SDCardManager.h"], text=True)
        self.header.write_text(self.original)
        (self.project / "scripts").mkdir()
        shutil.copyfile(root / "scripts/storage_sdk.patch", self.project / "scripts/storage_sdk.patch")

    def test_applies_once_and_preserves_unrelated_source(self):
        self.header.write_text(self.original + "\n// unrelated local work\n")
        self.assertEqual(apply_storage_patch(self.project), "applied")
        result = self.header.read_bytes()
        self.assertIn(b"filesystemType()", result)
        self.assertTrue(result.endswith(b"// unrelated local work\n"))
        self.assertEqual(apply_storage_patch(self.project), "already applied")
        self.assertEqual(self.header.read_bytes(), result)

    def test_cli_prepares_an_isolated_clean_sdk_for_host_tests(self):
        script = Path(__file__).resolve().parent / "patch_storage_sdk.py"
        result = subprocess.run([sys.executable, str(script), "--project", str(self.project)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("auto* device = sd.card();", self.header.read_text())

    def test_conflicting_sdk_is_not_modified(self):
        self.header.write_text(self.original.replace("bool ready() const;", "bool ready(int);"))
        before = self.header.read_bytes()
        with self.assertRaisesRegex(RuntimeError, "needs review"):
            apply_storage_patch(self.project)
        self.assertEqual(self.header.read_bytes(), before)

    def test_missing_patch_is_not_silently_ignored(self):
        (self.project / "scripts/storage_sdk.patch").unlink()
        with self.assertRaisesRegex(RuntimeError, "missing"):
            apply_storage_patch(self.project)
        self.assertEqual(self.header.read_text(), self.original)


if __name__ == "__main__":
    unittest.main()
