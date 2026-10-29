"""Fail-closed patch application without downloading or modifying PlatformIO dependencies."""
import difflib
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from patch_pngdec import FILES, active_library, apply_pngdec_patch, run_platformio


class PatchPngDecTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.library = self.root / "PNGdec"
        self.library.mkdir()
        self.patch_dir = self.root / "patches"
        self.patch_dir.mkdir()
        (self.library / "library.properties").write_text("name=PNGdec\nversion=1.1.6\n")
        self.before, self.after = {}, {}
        manifest = {"version": "1.1.6", "patch": "workspace.patch", "files": {}}
        chunks = []
        for i, name in enumerate(FILES):
            before, after = f"original {i}\n", f"patched {i}\n"
            self.before[name], self.after[name] = before, after
            path = self.library / name
            path.parent.mkdir(exist_ok=True)
            path.write_text(before)
            chunks.extend(difflib.unified_diff(before.splitlines(True), after.splitlines(True), fromfile="a/" + name, tofile="b/" + name))
            manifest["files"][name] = {"upstreamSHA256": self.digest(before), "patchedSHA256": self.digest(after)}
        patch = "".join(chunks)
        (self.patch_dir / "workspace.patch").write_text(patch)
        manifest["patchSHA256"] = self.digest(patch)
        self.manifest = manifest
        self.save_manifest()

    @staticmethod
    def digest(text):
        return hashlib.sha256(text.encode()).hexdigest()

    def save_manifest(self):
        (self.patch_dir / "manifest.json").write_text(json.dumps(self.manifest))

    def contents(self):
        return {name: (self.library / name).read_text() for name in FILES}

    def apply(self):
        return apply_pngdec_patch(self.library, self.patch_dir)

    def test_applies_exact_patch_and_is_idempotent(self):
        self.assertEqual(self.apply(), "applied")
        self.assertEqual(self.contents(), self.after)
        self.assertEqual(self.apply(), "already applied")
        self.assertEqual(self.contents(), self.after)

    def test_diverged_or_mixed_source_refuses_without_any_write(self):
        for replacement in ("unknown change\n", self.after[FILES[0]]):
            (self.library / FILES[0]).write_text(replacement)
            before = self.contents()
            with self.assertRaises(RuntimeError):
                self.apply()
            self.assertEqual(self.contents(), before)

    def test_wrong_version_missing_or_unexpected_manifest_refuses(self):
        for version in ("1.1.5", "1.1.60"):
            (self.library / "library.properties").write_text("version=" + version + "\n")
            with self.assertRaises(RuntimeError):
                self.apply()
        (self.library / "library.properties").write_text("version=1.1.6\n")
        self.manifest["files"]["src/extra.c"] = {}
        self.save_manifest()
        with self.assertRaises(RuntimeError):
            self.apply()
        self.assertEqual(self.contents(), self.before)

    def test_patch_tampering_refuses_before_any_write(self):
        with (self.patch_dir / "workspace.patch").open("a") as output:
            output.write("tampered\n")
        with self.assertRaises(RuntimeError):
            self.apply()
        self.assertEqual(self.contents(), self.before)

    def test_patch_syntax_and_postcondition_fail_closed(self):
        patch = "not a patch\n"
        (self.patch_dir / "workspace.patch").write_text(patch)
        self.manifest["patchSHA256"] = self.digest(patch)
        self.save_manifest()
        with self.assertRaises(RuntimeError):
            self.apply()
        self.assertEqual(self.contents(), self.before)

    def test_wrong_expected_result_refuses_build_after_patch(self):
        self.manifest["files"][FILES[0]]["patchedSHA256"] = "0" * 64
        self.save_manifest()
        with self.assertRaisesRegex(RuntimeError, "patched checksum mismatch"):
            self.apply()

    def test_path_escape_or_symlink_refuses(self):
        self.manifest["patch"] = "../workspace.patch"
        self.save_manifest()
        with self.assertRaises(RuntimeError):
            self.apply()
        self.manifest["patch"] = "workspace.patch"
        self.save_manifest()
        target = self.library / FILES[0]
        target.unlink()
        outside = self.root / "outside.h"
        outside.write_text(self.before[FILES[0]])
        target.symlink_to(outside)
        with self.assertRaises(RuntimeError):
            self.apply()

    def test_only_active_project_environment_and_clean_noop(self):
        deps = self.root / ".pio/libdeps"
        self.assertEqual(active_library(self.root, deps, "gh_release"), (deps / "gh_release/PNGdec").resolve())
        for name in ("../default", "", "default/../other"):
            with self.assertRaises(RuntimeError):
                active_library(self.root, deps, name)
        with self.assertRaises(RuntimeError):
            active_library(self.root, self.root.parent, "default")
        class Clean:
            def IsCleanTarget(self):
                return True
        run_platformio(Clean())


if __name__ == "__main__":
    unittest.main()
