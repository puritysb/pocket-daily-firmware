import importlib.util
import json
from pathlib import Path
import plistlib
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("build_apple", Path(__file__).resolve().parents[1] / "host/build_apple.py")
builder = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(builder)


class AppleHostArtifactTests(unittest.TestCase):
    def test_manifest_is_deterministic_and_detects_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "a.h").write_bytes(b"a")
            (root / "b.cpp").write_bytes(b"b")
            first = builder.manifest(root, ["b.cpp", "a.h", "a.h"])
            self.assertEqual(first, builder.manifest(root, ["a.h", "b.cpp"]))
            (root / "a.h").write_bytes(b"changed")
            self.assertNotEqual(builder.fingerprint(first), builder.fingerprint(builder.manifest(root, list(first))))

    def test_manifest_rejects_paths_and_symlinks(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "actual.h").write_bytes(b"a")
            (root / "alias.h").symlink_to(root / "actual.h")
            for name in ["../escape", "/absolute", "alias.h", "missing.h"]:
                with self.assertRaises(ValueError):
                    builder.manifest(root, [name])

    def package(self, root):
        package = root / "PocketUIHost.xcframework"
        package.mkdir()
        libraries = []
        for platform, variant, arches in [("macos", "", ["arm64", "x86_64"]),
                                         ("ios", "", ["arm64"]),
                                         ("ios", "simulator", ["arm64", "x86_64"])]:
            identifier = platform + ("-" + variant if variant else "")
            folder = package / identifier
            (folder / "Headers").mkdir(parents=True)
            (folder / "libpdui_host.a").write_bytes(b"fixture")
            (folder / "Headers/PocketUIHost.h").write_bytes(b"fixture")
            (folder / "Headers/module.modulemap").write_bytes(b"fixture")
            libraries.append(dict(LibraryIdentifier=identifier, SupportedPlatform=platform, SupportedPlatformVariant=variant,
                                  SupportedArchitectures=arches, LibraryPath="libpdui_host.a", HeadersPath="Headers"))
        (package / "Info.plist").write_bytes(plistlib.dumps({"AvailableLibraries": libraries}))
        return package, libraries

    def test_slice_validation_requires_all_platforms_and_no_duplicates(self):
        with tempfile.TemporaryDirectory() as directory:
            package, libraries = self.package(Path(directory))
            builder.validate_slices(package)
            for bad in [libraries[:2], libraries + [libraries[0]], [dict(libraries[0], SupportedArchitectures=["arm64"]), *libraries[1:]]]:
                (package / "Info.plist").write_bytes(plistlib.dumps({"AvailableLibraries": bad}))
                with self.assertRaises(ValueError):
                    builder.validate_slices(package)

    def test_dependencies_require_every_local_header_to_be_pinned(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build = root / "build"
            build.mkdir()
            header = root / "header with spaces.h"
            header.write_bytes(b"header")
            with self.assertRaises(ValueError):
                builder.validate_dependencies(root, build, {})
            escaped = str(header).replace(" ", "\\ ")
            (build / "test.o.d").write_text(f"test.o: {escaped}\n")
            builder.validate_dependencies(root, build, {header.name: "hash"})
            with self.assertRaises(ValueError):
                builder.validate_dependencies(root, build, {})

    def test_package_cannot_hide_added_symlinks_or_missing_library(self):
        with tempfile.TemporaryDirectory() as directory:
            package, _ = self.package(Path(directory))
            (package / "link").symlink_to("missing")
            with self.assertRaises(ValueError):
                builder.artifact_files(package)
            (package / "ios/libpdui_host.a").unlink()
            with self.assertRaises(ValueError):
                builder.validate_slices(package)

    def test_verification_rejects_source_or_artifact_drift(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            package, _ = self.package(root)
            state = {"commit": "test", "sha256": "source", "files": {}}
            files = builder.artifact_files(package)
            record = dict(schema=1, abi=1, source=state, artifactFiles=files, artifactSHA256=builder.fingerprint(files))
            (root / "PROVENANCE.json").write_text(json.dumps(record))
            with patch.object(builder, "source_state", return_value=state):
                builder.verify(root, root)
                (package / "extra").write_bytes(b"unexpected")
                with self.assertRaises(ValueError):
                    builder.verify(root, root)
            with patch.object(builder, "source_state", return_value={"changed": True}):
                with self.assertRaises(ValueError):
                    builder.verify(root, root)


if __name__ == "__main__":
    unittest.main()
