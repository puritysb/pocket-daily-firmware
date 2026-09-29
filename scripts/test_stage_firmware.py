"""The staged image name and manifest must match its embedded version."""
import tempfile
import unittest
from pathlib import Path

from stage_firmware import get_embedded_version


class EmbeddedVersionTest(unittest.TestCase):
    def image(self, payload):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        path = Path(folder.name) / "firmware.bin"
        path.write_bytes(payload)
        return path

    def test_beta_and_development_suffixes_are_preserved(self):
        beta = self.image(b"x\0CrossPoint version: 1.0.0-beta.1\0")
        self.assertEqual(get_embedded_version(beta), "1.0.0-beta.1")
        dev = self.image(b"CrossPoint version: 1.0.0-dev-main-abcd-w123\0")
        self.assertEqual(get_embedded_version(dev), "1.0.0-dev-main-abcd-w123")

    def test_missing_or_conflicting_identity_fails(self):
        with self.assertRaises(ValueError):
            get_embedded_version(self.image(b"no version"))
        with self.assertRaises(ValueError):
            get_embedded_version(self.image(
                b"CrossPoint version: 1.0.0\0CrossPoint version: 1.0.1\0"))


if __name__ == "__main__":
    unittest.main()
