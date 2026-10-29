"""Apply only the reviewed PNGdec 1.1.6 workspace patch in the active environment."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

FILES = ("src/PNGdec.h", "src/PNGdec.cpp", "src/png.inl")
VERSION = "1.1.6"


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def apply_pngdec_patch(library: Path, patch_dir: Path) -> str:
    library = library.resolve()
    manifest = json.loads((patch_dir / "manifest.json").read_text())
    if manifest.get("version") != VERSION or set(manifest.get("files", {})) != set(FILES):
        raise RuntimeError("PNGdec patch manifest requires review")
    patch_name = manifest.get("patch", "")
    if Path(patch_name).name != patch_name or not patch_name.endswith(".patch"):
        raise RuntimeError("Invalid PNGdec patch filename")
    patch = (patch_dir / patch_name).resolve()
    if sha256(patch) != manifest.get("patchSHA256"):
        raise RuntimeError("PNGdec patch checksum mismatch")
    properties = (library / "library.properties").read_text().splitlines()
    if [line.strip() for line in properties if line.startswith("version=")] != ["version=" + VERSION]:
        raise RuntimeError("PNGdec version changed; review the patch before building")
    actual = {}
    for name in FILES:
        source = library / name
        if source.is_symlink() or not source.resolve().is_relative_to(library):
            raise RuntimeError("PNGdec source must remain inside its library directory")
        actual[name] = sha256(source)
    if all(actual[name] == manifest["files"][name]["patchedSHA256"] for name in FILES):
        return "already applied"
    if not all(actual[name] == manifest["files"][name]["upstreamSHA256"] for name in FILES):
        raise RuntimeError("PNGdec source differs from reviewed upstream/patched files; no changes made")
    checked = subprocess.run(["git", "apply", "--check", str(patch)], cwd=library, capture_output=True)
    if checked.returncode:
        raise RuntimeError("PNGdec reviewed patch did not apply cleanly; no changes made")
    subprocess.run(["git", "apply", str(patch)], cwd=library, check=True, capture_output=True)
    if not all(sha256(library / name) == manifest["files"][name]["patchedSHA256"] for name in FILES):
        raise RuntimeError("PNGdec patched checksum mismatch; refusing to build")
    return "applied"


def active_library(project: Path, libdeps: Path, environment: str) -> Path:
    if not re.fullmatch(r"[A-Za-z0-9_-]+", environment):
        raise RuntimeError("Invalid PlatformIO environment name")
    target = (libdeps / environment / "PNGdec").resolve()
    if not target.is_relative_to(project.resolve()):
        raise RuntimeError("PNGdec patch requires a project-local active dependency")
    return target


def run_platformio(env) -> None:
    # Clean does not compile and may run before dependencies have been installed.
    if env.IsCleanTarget():
        return
    project = Path(env["PROJECT_DIR"])
    library = active_library(project, Path(env.subst("$PROJECT_LIBDEPS_DIR")), env["PIOENV"])
    print("PNGdec workspace patch: " + apply_pngdec_patch(library, project / "scripts/pngdec_patches"))


if "Import" in globals():
    Import("env")  # noqa: F821 - PlatformIO/SCons
    run_platformio(env)  # noqa: F821
elif __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--patch-dir", type=Path, default=Path(__file__).resolve().parent / "pngdec_patches")
    args = parser.parse_args()
    print(apply_pngdec_patch(args.library, args.patch_dir))
