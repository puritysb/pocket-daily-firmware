import configparser
from pathlib import Path
import tempfile
import sys
import types
import unittest
from unittest.mock import patch

from check_firmware import native_command, prepare, run_platformio


class CheckFirmwareTests(unittest.TestCase):
    def test_preserves_build_inputs_without_local_overrides(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            original = """[platformio]
extra_configs = platformio.local.ini
[base]
platform_packages = tool-scons=https://example.invalid/scons.zip
check_flags = --enable=all --inline-suppr
build_flags = -std=gnu++2a
extra_scripts = pre:scripts/patch_storage_sdk.py
[env:default]
extends = base
"""
            (root / "platformio.ini").write_text(original)
            generated = prepare(root)
            config = configparser.ConfigParser(interpolation=None)
            config.read(generated)
            self.assertFalse(config.has_option("platformio", "extra_configs"))
            self.assertEqual(config["base"]["check_flags"], "--enable=all --inline-suppr")
            self.assertEqual(config["base"]["extra_scripts"], "pre:scripts/patch_storage_sdk.py")
            self.assertEqual(config["base"]["platform_packages"], "tool-scons=https://example.invalid/scons.zip")
            self.assertEqual(config["env:default"]["extends"], "base")
            self.assertEqual((root / "platformio.ini").read_text(), original)

    def test_native_command_preserves_defect_handling_and_arguments(self):
        sentinel = object()
        def original(tool, language, source):
            self.assertIs(tool, sentinel)
            self.assertEqual(language, "c++")
            return ["/bundled/cppcheck", "--error-exitcode=3", "--enable=all", "-DTEST=1", source]
        command = native_command(original, "/native tool's/cppcheck")
        self.assertEqual(command(sentinel, "c++", "source file.cpp"),
                         ["/native tool's/cppcheck", "--error-exitcode=3", "--enable=all", "-DTEST=1", "source file.cpp"])

    def test_platformio_failure_reaches_the_parent_process(self):
        for result in (0, 1, 3):
            with self.subTest(result=result):
                modules = {name: types.ModuleType(name) for name in (
                    "platformio", "platformio.check", "platformio.check.tools",
                    "platformio.check.tools.cppcheck", "platformio.__main__")}
                class Checker:
                    configure_command = staticmethod(lambda *args: ["bundled-cppcheck"])
                modules["platformio.check.tools.cppcheck"].CppcheckCheckTool = Checker
                def main():
                    self.assertEqual(sys.argv, ["pio", "check", "--fail-on-defect", "low"])
                    return result
                modules["platformio.__main__"].main = main
                with patch.dict(sys.modules, modules), patch.object(sys, "argv", ["test"]):
                    with self.assertRaises(SystemExit) as raised:
                        run_platformio("/native/cppcheck", ["check", "--fail-on-defect", "low"])
                    self.assertEqual(raised.exception.code, result)


if __name__ == "__main__":
    unittest.main()
