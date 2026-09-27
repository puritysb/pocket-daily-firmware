import configparser
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

from check_firmware import prepare


class CheckFirmwareTests(unittest.TestCase):
    def test_preserves_analysis_and_build_inputs_without_local_overrides(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            original = """[platformio]
extra_configs = platformio.local.ini
[base]
check_flags = --enable=all --inline-suppr
build_flags = -std=gnu++2a
extra_scripts = pre:scripts/patch_storage_sdk.py
[env:default]
extends = base
"""
            (root / "platformio.ini").write_text(original)
            executable = root / "native tool's cppcheck"
            executable.write_text('#!/bin/sh\nprintf "%s\\n" "$@"\n')
            executable.chmod(0o755)
            generated = prepare(root, executable, "2.11.0")
            config = configparser.ConfigParser(interpolation=None)
            config.read(generated)
            self.assertFalse(config.has_option("platformio", "extra_configs"))
            self.assertEqual(config["base"]["check_flags"], "--enable=all --inline-suppr")
            self.assertEqual(config["base"]["extra_scripts"], "pre:scripts/patch_storage_sdk.py")
            self.assertEqual(config["env:default"]["extends"], "base")
            self.assertEqual((root / "platformio.ini").read_text(), original)
            package = generated.parent / "tool-cppcheck"
            self.assertEqual(json.loads((package / "package.json").read_text())["version"], "2.11.0")
            result = subprocess.check_output([str(package / "cppcheck"), "--version", "two words"], text=True)
            self.assertEqual(result.splitlines(), ["--version", "two words"])


if __name__ == "__main__":
    unittest.main()
