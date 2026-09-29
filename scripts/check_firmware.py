#!/usr/bin/env python3
"""Run strict PlatformIO analysis with a verified native cppcheck 2.11.

The pioarduino Core 6.2 analyzer hardcodes a bundled executable, ignoring its
platform_packages override. Replace only argv[0] in this process; keep all
PlatformIO compiler defines, file selection, diagnostics and failure handling.
No installed tool or platform files are modified.
"""
import argparse
import configparser
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys


def prepare(project: Path) -> Path:
    output = project / "build" / "system-cppcheck"
    output.mkdir(parents=True, exist_ok=True)
    config = configparser.ConfigParser(interpolation=None)
    with (project / "platformio.ini").open() as source:
        config.read_file(source)
    config.remove_option("platformio", "extra_configs")
    generated = output / "platformio.ini"
    with generated.open("w") as target:
        config.write(target)
    return generated


def native_command(original, executable):
    def configure(tool, language, src_file):
        command = original(tool, language, src_file)
        command[0] = str(executable)
        return command
    return configure


def run_platformio(executable, arguments):
    from platformio.check.tools.cppcheck import CppcheckCheckTool
    from platformio.__main__ import main as pio_main

    CppcheckCheckTool.configure_command = native_command(
        CppcheckCheckTool.configure_command, executable)
    sys.argv = ["pio", *arguments]
    pio_main()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cppcheck", default="cppcheck", help="Path to native cppcheck 2.11")
    args = parser.parse_args()
    executable = shutil.which(args.cppcheck)
    if not executable:
        raise SystemExit("Install cppcheck before running scripts/check_firmware.py")
    reported = subprocess.check_output([executable, "--version"], text=True).strip()
    if not re.fullmatch(r"Cppcheck 2\.11(?:\.0)?", reported):
        raise SystemExit("Cppcheck 2.11 is required; found " + reported)
    project = Path(__file__).resolve().parent.parent
    generated = prepare(project)
    info = json.loads(subprocess.check_output(
        [str(project / "scripts/pio.sh"), "system", "info", "--json-output"], text=True))
    print("Analyzing with " + reported, flush=True)
    subprocess.run([info["python_exe"]["value"], str(Path(__file__).resolve()),
                    "--run-platformio", str(Path(executable).resolve()),
                    "check", "-d", str(project), "-c", str(generated), "-e", "default",
                    "--fail-on-defect", "low", "--fail-on-defect", "medium",
                    "--fail-on-defect", "high"], check=True)


if __name__ == "__main__":
    if len(sys.argv) > 2 and sys.argv[1] == "--run-platformio":
        run_platformio(sys.argv[2], sys.argv[3:])
    else:
        main()
