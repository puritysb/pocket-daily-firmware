#!/usr/bin/env python3
"""Run strict PlatformIO analysis with an installed, native cppcheck executable.

Keeps PlatformIO's compiler defines, source selection and defect handling. The
small package adapter avoids its bundled tool's loader/runtime dependencies.
Generated files stay in build/; platformio.ini and global packages are untouched.
"""
import argparse
import configparser
import json
from pathlib import Path
import re
import shlex
import shutil
import subprocess


def prepare(project: Path, executable: Path, version: str) -> Path:
    output = project / "build" / "system-cppcheck"
    package = output / "tool-cppcheck"
    package.mkdir(parents=True, exist_ok=True)
    (package / "package.json").write_text(json.dumps({
        "name": "tool-cppcheck", "version": version, "system": ["*"],
        "description": "Adapter for the runner's installed cppcheck"
    }) + "\n")
    launcher = package / "cppcheck"
    launcher.write_text("#!/bin/sh\nexec " + shlex.quote(str(executable)) + ' "$@"\n')
    launcher.chmod(0o755)
    config = configparser.ConfigParser(interpolation=None)
    with (project / "platformio.ini").open() as source:
        config.read_file(source)
    # Analyze the checked-in default profile, never per-machine overrides.
    config.remove_option("platformio", "extra_configs")
    packages = config.get("env:default", "platform_packages", fallback="")
    if not packages and config.has_option("base", "platform_packages"):
        packages = "${base.platform_packages}"
    config.set("env:default", "platform_packages",
               packages + "\ntool-cppcheck=" + package.resolve().as_uri())
    generated = output / "platformio.ini"
    with generated.open("w") as target:
        config.write(target)
    return generated


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cppcheck", default="cppcheck", help="Path to native cppcheck 2.11")
    args = parser.parse_args()
    executable = shutil.which(args.cppcheck)
    if not executable:
        raise SystemExit("Install cppcheck before running scripts/check_firmware.py")
    reported = subprocess.check_output([executable, "--version"], text=True).strip()
    match = re.fullmatch(r"Cppcheck (\d+\.\d+(?:\.\d+)?)", reported)
    if not match or match[1] not in ("2.11", "2.11.0"):
        raise SystemExit("Cppcheck 2.11 is required (same baseline as PlatformIO); found " + reported)
    version = match[1] if match[1].count(".") == 2 else match[1] + ".0"
    project = Path(__file__).resolve().parent.parent
    generated = prepare(project, Path(executable).resolve(), version)
    print("Analyzing with " + reported, flush=True)
    subprocess.run([str(project / "scripts/pio.sh"), "check", "-d", str(project),
                    "-c", str(generated), "-e", "default", "--fail-on-defect", "low",
                    "--fail-on-defect", "medium", "--fail-on-defect", "high"], check=True)


if __name__ == "__main__":
    main()
