#!/bin/bash
# Keep HybridCompile's mutable SDK packages separate from normal builds.
set -euo pipefail
cd "$(dirname "$0")/.."
export PLATFORMIO_PACKAGES_DIR="$PWD/build/ble-standby/packages"
if [[ ! -d "$PLATFORMIO_PACKAGES_DIR/framework-arduinoespressif32-libs" ]]; then
  python3 - <<'INIT'
import os
from pathlib import Path
import shutil
import subprocess
import sys
source = Path(os.environ.get("PLATFORMIO_CORE_DIR", str(Path.home() / ".platformio"))) / "packages"
target = Path(os.environ["PLATFORMIO_PACKAGES_DIR"])
if not (source / "framework-arduinoespressif32-libs").is_dir():
    raise SystemExit("Run ./scripts/pio.sh run -e default once to install the normal SDK first")
target.mkdir(parents=True, exist_ok=True)
for package in source.iterdir():
    destination = target / package.name
    if package.name.startswith(".") or destination.exists():
        continue
    if sys.platform == "darwin":
        # APFS copy-on-write clones avoid doubling the toolchains' disk usage.
        subprocess.run(["cp", "-cR", str(package), str(destination)], check=True)
    elif package.is_dir():
        shutil.copytree(package, destination, symlinks=True)
    else:
        shutil.copy2(package, destination)
INIT
fi
exec ./scripts/pio.sh run -e ble_standby "$@"
