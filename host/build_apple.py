#!/usr/bin/env python3
"""Build local Apple host-renderer artifacts; never access a reader or publish.

Each run owns a fresh build directory. No existing artifact is overwritten.
Source and package manifests pin dirty-tree bytes, not just a potentially stale
Git commit. This is provenance/integrity checking, not a signature or trust root.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import shlex
import subprocess
import tempfile

SOURCE_ROOTS = (
    "host", "src/pocket_daily", "src/models.h", "lib/GfxRenderer", "lib/EpdFont",
    "lib/Memory", "lib/MiniBidi", "lib/Utf8", "lib/InflateReader", "lib/uzlib",
    "src/fontIds.h", "test/gfx_host/HostAbiSwiftCheck.swift",
)
SOURCE_SUFFIXES = {".c", ".cpp", ".h", ".hpp", ".cmake", ".py", ".modulemap", ".t", ".swift"}
PLATFORMS = (
    ("macos", "macosx", "Darwin", "14.0", ("arm64", "x86_64")),
    ("ios", "iphoneos", "iOS", "17.0", ("arm64",)),
    ("simulator", "iphonesimulator", "iOS", "17.0", ("arm64", "x86_64")),
)


def command(*args: str, cwd: Path | None = None) -> str:
    return subprocess.check_output(args, cwd=cwd, text=True).strip()


def digest_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def manifest(root: Path, paths: list[str]) -> dict[str, str]:
    result = {}
    for name in sorted(set(paths)):
        path = root / name
        if Path(name).is_absolute() or ".." in Path(name).parts or path.is_symlink():
            raise ValueError(f"Unsafe manifest path: {name}")
        if not path.resolve().is_relative_to(root.resolve()) or not path.is_file():
            raise ValueError(f"Missing or external manifest file: {name}")
        result[name] = digest_file(path)
    return result


def fingerprint(files: dict[str, str]) -> str:
    encoded = json.dumps(files, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(encoded).hexdigest()


def source_state(root: Path) -> dict:
    names = command("git", "ls-files", "-z", "--cached", "--others", "--exclude-standard",
                    "--", *SOURCE_ROOTS, cwd=root).split("\0")
    # Ignore generated build output and prose, but include all native source and
    # build/header definitions in the renderer's dependency directories.
    names = [name for name in names if name and (root / name).exists() and
             (Path(name).suffix in SOURCE_SUFFIXES or Path(name).name == "CMakeLists.txt")]
    files = manifest(root, names)
    if "host/CMakeLists.txt" not in files or "host/include/PocketUIHost.h" not in files:
        raise ValueError("Not a Pocket UI host source tree")
    return {"commit": command("git", "rev-parse", "HEAD", cwd=root),
            "sha256": fingerprint(files), "files": files}


def artifact_files(package: Path) -> dict[str, str]:
    paths = list(package.rglob("*"))
    if any(path.is_symlink() for path in paths):
        raise ValueError("Symlinks are not allowed in host artifacts")
    return manifest(package, [str(path.relative_to(package)) for path in paths if path.is_file()])


def validate_dependencies(root: Path, build_dir: Path, files: dict[str, str]) -> None:
    root = root.resolve()
    dependencies = list(build_dir.rglob("*.o.d"))
    if not dependencies:
        raise ValueError("Missing compiler dependency records")
    for record in dependencies:
        body = record.read_text().replace("\\\n", "")
        for name in shlex.split(body.split(":", 1)[1]):
            path = Path(name)
            if not path.is_absolute():
                path = build_dir / path
            path = path.resolve()
            if path.is_relative_to(root):
                relative = str(path.relative_to(root))
                if relative not in files:
                    raise ValueError(f"Unpinned local compiler dependency: {relative}")


def validate_slices(package: Path) -> None:
    with (package / "Info.plist").open("rb") as stream:
        info = plistlib.load(stream)
    expected = {("macos", ""): {"arm64", "x86_64"}, ("ios", ""): {"arm64"},
                ("ios", "simulator"): {"arm64", "x86_64"}}
    found = {}
    for item in info.get("AvailableLibraries", []):
        key = (item.get("SupportedPlatform"), item.get("SupportedPlatformVariant", ""))
        if key in found or key not in expected:
            raise ValueError("Duplicate/unexpected XCFramework platform")
        found[key] = set(item.get("SupportedArchitectures", []))
        if item.get("LibraryPath") != "libpdui_host.a" or item.get("HeadersPath") != "Headers":
            raise ValueError("Unexpected XCFramework library/header path")
        identifier = item.get("LibraryIdentifier", "")
        if not identifier or Path(identifier).name != identifier or identifier in {".", ".."}:
            raise ValueError("Unsafe XCFramework library identifier")
        for relative in ["libpdui_host.a", "Headers/PocketUIHost.h", "Headers/module.modulemap"]:
            if not (package / identifier / relative).is_file():
                raise ValueError("Missing XCFramework library or public header")
    if found != expected:
        raise ValueError("Missing/wrong XCFramework architectures")


def verify(root: Path, output: Path) -> dict:
    with (output / "PROVENANCE.json").open() as stream:
        provenance = json.load(stream)
    if provenance.get("schema") != 1 or provenance.get("abi") != 1:
        raise ValueError("Unsupported provenance/ABI")
    if provenance["source"] != source_state(root):
        raise ValueError("Renderer sources/commit changed; rebuild the artifact")
    package = output / "PocketUIHost.xcframework"
    validate_slices(package)
    files = artifact_files(package)
    if files != provenance["artifactFiles"] or fingerprint(files) != provenance["artifactSHA256"]:
        raise ValueError("Host artifact changed or is incomplete")
    return provenance


def build(root: Path) -> Path:
    before = source_state(root)
    (root / "build").mkdir(exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix="apple-host-", dir=root / "build"))
    print(f"Build directory: {output}", flush=True)
    sdk_info = {}
    libraries = []
    for name, sdk, system, minimum, architectures in PLATFORMS:
        sdk_path = Path(command("xcrun", "--sdk", sdk, "--show-sdk-path"))
        sdk_info[name] = {"version": command("xcrun", "--sdk", sdk, "--show-sdk-version"),
                          "minimumOS": minimum, "architectures": list(architectures)}
        destination = output / name
        print(f"Building {name}: {','.join(architectures)}", flush=True)
        with (output / f"{name}.log").open("w") as log:
            subprocess.run([
                "cmake", "-S", str(root / "host"), "-B", str(destination),
                "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_SYSTEM_NAME={system}",
                f"-DCMAKE_OSX_SYSROOT={sdk_path}", f"-DCMAKE_OSX_DEPLOYMENT_TARGET={minimum}",
                f"-DCMAKE_OSX_ARCHITECTURES={';'.join(architectures)}",
                "-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY",
                f"-DZLIB_INCLUDE_DIR={sdk_path / 'usr/include'}",
                f"-DZLIB_LIBRARY={sdk_path / 'usr/lib/libz.tbd'}",
            ], check=True, stdout=log, stderr=subprocess.STDOUT)
            subprocess.run(["cmake", "--build", str(destination), "--parallel", "2"],
                           check=True, stdout=log, stderr=subprocess.STDOUT)
        library = destination / "libpdui_host.a"
        validate_dependencies(root, destination, before["files"])
        if set(command("xcrun", "lipo", "-archs", str(library)).split()) != set(architectures):
            raise ValueError(f"Wrong archive architectures: {name}")
        # A static archive alone does not prove that the platform/ABI can link.
        # Compile a real Swift consumer for every slice without running iOS code
        # or contacting a reader. Device/simulator signing is not required.
        with (output / f"{name}.log").open("a") as log:
            for architecture in architectures:
                os_target = f"macosx{minimum}" if name == "macos" else f"ios{minimum}"
                triple = f"{architecture}-apple-{os_target}" + ("-simulator" if name == "simulator" else "")
                subprocess.run([
                    "xcrun", "--sdk", sdk, "swiftc", "-sdk", str(sdk_path), "-target", triple,
                    "-I", str(root / "host/include"), str(root / "test/gfx_host/HostAbiSwiftCheck.swift"),
                    str(library), "-lc++", "-lz", "-o", str(destination / f"swift-link-{architecture}"),
                ], check=True, stdout=log, stderr=subprocess.STDOUT)
        sdk_info[name]["swiftLinkVerified"] = True
        libraries.extend(["-library", str(library), "-headers", str(root / "host/include")])
    subprocess.run(["xcodebuild", "-create-xcframework", *libraries,
                    "-output", str(output / "PocketUIHost.xcframework")], check=True)
    if source_state(root) != before:
        raise ValueError("Sources changed during build; refusing to seal the artifact")
    package = output / "PocketUIHost.xcframework"
    validate_slices(package)
    files = artifact_files(package)
    provenance = {"schema": 1, "abi": 1, "source": before, "platforms": sdk_info,
                  "xcode": command("xcodebuild", "-version"), "cmake": command("cmake", "--version"),
                  "compiler": command("xcrun", "clang", "--version"),
                  "artifactFiles": files, "artifactSHA256": fingerprint(files)}
    with (output / "PROVENANCE.json").open("x") as stream:
        json.dump(provenance, stream, indent=2, sort_keys=True)
        stream.write("\n")
    verify(root, output)
    print(f"Verified artifact: {output}", flush=True)
    return output


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify", type=Path, help="Verify an existing build against current sources")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    if args.verify:
        verify(root, args.verify.resolve())
        print("Host artifact and source fingerprints match.")
    else:
        build(root)


if __name__ == "__main__":
    main()
