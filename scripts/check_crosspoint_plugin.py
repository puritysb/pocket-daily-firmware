"""Validate the Pocket Daily installer plugin for stock CrossPoint.

Checks the Plugin Hub payload contract and the stock 2.0 catalog fields the
plugin depends on (distribution/crosspoint-plugin/README.md). Host-only: it
does not prove a download or install on a reader.
"""
from pathlib import Path
import json
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
DIST = Path("distribution/crosspoint-plugin")
REPOSITORY = "puritysb/pocket-daily-firmware"
STAGED_NAME = "pocket-daily.bin"  # companion PocketModel.crossPointStagedFirmwareName
NAME = re.compile(r"^[a-z0-9]+(?:-[a-z0-9]+)*$")
SEMVER = re.compile(r"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$")
README_LIMIT = 16 * 1024  # stock on-device README limit


def resolve(value, dotted):
    """Stock catalog dotted-path lookup; numeric segments index arrays."""
    if dotted == "":
        return value
    for part in dotted.split("."):
        if isinstance(value, list) and part.isdigit() and int(part) < len(value):
            value = value[int(part)]
        elif isinstance(value, dict) and part in value:
            value = value[part]
        else:
            return None
    return value


def release_url(version):
    return f"https://github.com/{REPOSITORY}/releases/download/pocket-v{version}/firmware.bin"


def problems(root=ROOT):
    errors = []
    dist = root / DIST
    payloads = sorted(p for p in dist.iterdir() if p.is_dir() and p.name.endswith(".crosspoint-plugin"))
    if len(payloads) != 1:
        return [f"expected exactly one *.crosspoint-plugin folder, found {len(payloads)}"]
    payload = payloads[0]
    stem = payload.name[: -len(".crosspoint-plugin")]

    manifest = json.loads((payload / "manifest.json").read_text())
    if manifest.get("name") != stem:
        errors.append(f"manifest name {manifest.get('name')!r} does not match folder {stem!r}")
    if not NAME.match(manifest.get("name", "")):
        errors.append("manifest name must be lowercase letters, digits and hyphens")
    if not SEMVER.match(manifest.get("version", "")):
        errors.append("manifest version must be MAJOR.MINOR.PATCH")
    declared = manifest.get("files", [])
    actual = sorted(p.relative_to(payload).as_posix() for p in payload.rglob("*") if p.is_file())
    if sorted(declared) != actual:
        errors.append(f"manifest files {sorted(declared)} differ from payload {actual}")
    if "manifest.json" not in declared or not {"device.json", "plugin.js"} & set(declared):
        errors.append("payload needs manifest.json and device.json or plugin.js")
    # Plugin Hub's advisory hardware filter (discovery only; stock ignores it).
    # Pocket Daily's single image runs only on boards tagged x4 (X3 and X4).
    if manifest.get("compatibility", {}).get("devices") != ["x4"]:
        errors.append("manifest compatibility.devices must be [\"x4\"]")
    for entry in declared:
        if entry.startswith("/") or "\\" in entry or ".." in entry.split("/"):
            errors.append(f"unsafe payload path {entry!r}")

    device = json.loads((payload / "device.json").read_text())
    browse = device.get("browse", {})
    index_path = (DIST / "releases.json").as_posix()
    expected_index = f"https://raw.githubusercontent.com/{REPOSITORY}/main/{index_path}"
    if browse.get("url") != expected_index:
        errors.append(f"browse.url must be {expected_index}")
    if browse.get("format", "json") != "json":
        errors.append("browse.format must be json")
    if "{page}" in browse.get("url", "") or "{offset}" in browse.get("url", ""):
        errors.append("the static release index must page locally")
    download = device.get("download", {})
    # Stock 2.0 only defaults the file URL to {url} for XML lists.
    if download.get("url") != "{url}":
        errors.append("download.url must be {url} for a JSON catalog")
    if download.get("filename") != STAGED_NAME:
        errors.append(f"download.filename must be {STAGED_NAME}")
    if download.get("dest_dir", ""):
        errors.append("download.dest_dir must be empty (SD root, where the SD updater looks)")
    for key in ("headers", "username", "password", "url_path", "sidecar"):
        if key in download:
            errors.append(f"download.{key} is not used by a public release download")
    if "token" in device or "auth" in device:
        errors.append("the installer needs no token or sign-in")

    readme = (payload / "README.md").read_bytes()
    if len(readme) > README_LIMIT:
        errors.append("README.md exceeds the 16 KiB on-device limit")
    if any(byte > 0x7F for byte in readme):
        errors.append("README.md must be ASCII so every stock UI font can show it")

    index = json.loads((dist / "releases.json").read_text())
    items = resolve(index, browse.get("items", ""))
    if not isinstance(items, list):
        return errors + ["browse.items does not resolve to a list in releases.json"]
    fields = browse.get("fields", {})
    compatibility = browse.get("compatibility", {})
    seen = set()
    for number, item in enumerate(items):
        label = f"releases[{number}]"
        version = resolve(item, fields.get("id", ""))
        if not isinstance(version, str) or not SEMVER.match(version):
            errors.append(f"{label}: id must be a MAJOR.MINOR.PATCH product version")
            continue
        if version in seen:
            errors.append(f"{label}: duplicate version {version}")
        seen.add(version)
        if not resolve(item, fields.get("title", "")):
            errors.append(f"{label}: missing title (stock drops untitled rows)")
        if resolve(item, fields.get("url", "")) != release_url(version):
            errors.append(f"{label}: url must be {release_url(version)}")
        family = resolve(item, compatibility.get("firmware_family", ""))
        if family is not None and "crosspoint" not in str(family).split(","):
            errors.append(f"{label}: firmware_family must allow crosspoint")
    return errors


def main():
    errors = problems()
    for error in errors:
        print(f"crosspoint plugin: {error}")
    if errors:
        return 1
    print("CrossPoint installer plugin checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
