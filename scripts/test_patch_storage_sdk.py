import shutil
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
        relative = Path("open-x4-sdk/libs/hardware/SDCardManager/include/SDCardManager.h")
        self.header = self.project / relative
        self.header.parent.mkdir(parents=True)
        addition = ("  // Pocket HAL-only query: 0 unavailable, 12/16/32 FAT, 64 exFAT. No scan.\n"
                    "  uint8_t filesystemType() const { return initialized ? sd.fatType() : 0; }\n")
        self.original = (root / relative).read_text().replace(addition, "")
        self.header.write_text(self.original)
        (self.project / "scripts").mkdir()
        shutil.copyfile(root / "scripts/storage_sdk.patch", self.project / "scripts/storage_sdk.patch")

    def test_applies_once_and_preserves_unrelated_source(self):
        self.header.write_text(self.original + "\n// unrelated local work\n")
        self.assertEqual(apply_storage_patch(self.project), "applied")
        result = self.header.read_bytes()
        self.assertIn(b"filesystemType() const", result)
        self.assertTrue(result.endswith(b"// unrelated local work\n"))
        self.assertEqual(apply_storage_patch(self.project), "already applied")
        self.assertEqual(self.header.read_bytes(), result)

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
