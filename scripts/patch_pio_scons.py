"""Align the installed platform's SCons version validator with Core 6.2.0.

Run after pio pkg install and before starting SCons. Platform 55.03.311
otherwise deletes its active 4.11 package because package-version still says
4.8, even when platform_packages pins the correct archive URL.
"""
import argparse
import json
from pathlib import Path

VERSION = "4.41101.0"
URL = "https://github.com/pioarduino/scons/releases/download/4.11.1/scons-local-4.11.1.tar.gz"
OLD_URL = "https://github.com/pioarduino/registry/releases/download/0.0.1/scons-4.8.1.zip"


def patch(manifest):
    original = manifest.read_text()
    data = json.loads(original)
    tool = data["packages"]["tool-scons"]
    if tool.get("package-version") not in ("4.40801.0", VERSION) or tool.get("version") not in (OLD_URL, URL):
        raise RuntimeError("Unknown platform SCons metadata; review before patching")
    if tool["package-version"] == VERSION and tool["version"] == URL:
        return False
    tool["package-version"] = VERSION
    tool["version"] = URL
    manifest.write_text(json.dumps(data, indent=2) + "\n")
    return True


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    args = parser.parse_args()
    print("SCons platform metadata:", "aligned" if patch(args.manifest) else "already aligned")
