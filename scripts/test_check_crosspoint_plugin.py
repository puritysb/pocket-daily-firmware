import json
import shutil
import tempfile
import unittest
from pathlib import Path

import check_crosspoint_plugin as check

PAYLOAD = "pocket-daily-installer.crosspoint-plugin"


class CrossPointPluginTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)
        shutil.copytree(check.ROOT / check.DIST, self.tmp / check.DIST)
        self.dist = self.tmp / check.DIST

    def edit(self, relative, change):
        path = self.dist / relative
        data = json.loads(path.read_text())
        change(data)
        path.write_text(json.dumps(data))

    def add_release(self, version, **overrides):
        entry = {"title": f"Pocket Daily {version}", "subtitle": "X3/X4", "version": version,
                 "url": check.release_url(version), "notes": "",
                 "compatibility": {"firmware_family": "crosspoint", "plugin_api_min": 1}}
        entry.update(overrides)
        self.edit("releases.json", lambda d: d["releases"].append(entry))

    def test_repository_payload_passes(self):
        self.assertEqual(check.problems(), [])

    def test_a_valid_release_entry_passes(self):
        self.add_release("1.0.0")
        self.assertEqual(check.problems(self.tmp), [])

    def test_release_url_must_be_that_versions_asset(self):
        self.add_release("1.0.0", url=check.release_url("1.0.1"))
        self.assertTrue(any("url must be" in e for e in check.problems(self.tmp)))

    def test_release_needs_title_semver_and_unique_version(self):
        self.add_release("1.0.0", title="")
        self.add_release("1.0", url=check.release_url("1.0"))
        self.add_release("1.0.0")
        errors = check.problems(self.tmp)
        self.assertTrue(any("missing title" in e for e in errors))
        self.assertTrue(any("MAJOR.MINOR.PATCH product version" in e for e in errors))
        self.assertTrue(any("duplicate version" in e for e in errors))

    def test_release_family_must_allow_crosspoint(self):
        self.add_release("1.0.0", compatibility={"firmware_family": "other"})
        self.assertTrue(any("firmware_family" in e for e in check.problems(self.tmp)))

    def test_download_must_land_where_the_sd_updater_and_app_expect(self):
        self.edit(f"{PAYLOAD}/device.json", lambda d: d["download"].update(filename="firmware.bin"))
        self.assertTrue(any("download.filename" in e for e in check.problems(self.tmp)))
        self.edit(f"{PAYLOAD}/device.json", lambda d: d["download"].update(filename="pocket-daily.bin",
                                                                       dest_dir="/Pocket"))
        self.assertTrue(any("dest_dir" in e for e in check.problems(self.tmp)))

    def test_json_catalog_must_name_the_item_url(self):
        self.edit(f"{PAYLOAD}/device.json", lambda d: d["download"].pop("url"))
        self.assertTrue(any("download.url" in e for e in check.problems(self.tmp)))

    def test_browse_url_must_point_at_the_index_on_main(self):
        self.edit(f"{PAYLOAD}/device.json",
                  lambda d: d["browse"].update(url=d["browse"]["url"].replace("/main/", "/develop/")))
        self.assertTrue(any("browse.url" in e for e in check.problems(self.tmp)))

    def test_manifest_name_version_and_files(self):
        self.edit(f"{PAYLOAD}/manifest.json", lambda d: d.update(name="Pocket_Daily", version="1.0"))
        errors = check.problems(self.tmp)
        self.assertTrue(any("does not match folder" in e for e in errors))
        self.assertTrue(any("lowercase" in e for e in errors))
        self.assertTrue(any("MAJOR.MINOR.PATCH" in e for e in errors))
        (self.dist / PAYLOAD / "extra.txt").write_text("x")
        self.assertTrue(any("differ from payload" in e for e in check.problems(self.tmp)))

    def test_hub_discovery_is_limited_to_the_x4_image(self):
        self.edit(f"{PAYLOAD}/manifest.json", lambda d: d["compatibility"].update(devices=["x4", "x4pro"]))
        self.assertTrue(any("compatibility.devices" in e for e in check.problems(self.tmp)))

    def test_readme_must_be_ascii_and_bounded(self):
        (self.dist / PAYLOAD / "README.md").write_text("Pocket Daily → install\n")
        self.assertTrue(any("ASCII" in e for e in check.problems(self.tmp)))
        (self.dist / PAYLOAD / "README.md").write_text("x" * (check.README_LIMIT + 1))
        self.assertTrue(any("16 KiB" in e for e in check.problems(self.tmp)))

    def test_staged_name_matches_the_companion_app(self):
        model = check.ROOT.parent / "pocket-daily" / "Sources" / "PocketModel.swift"
        if not model.exists():
            self.skipTest("sibling pocket-daily checkout not present")
        self.assertIn(f'crossPointStagedFirmwareName = "{check.STAGED_NAME}"', model.read_text())

    def test_dotted_paths_index_arrays_like_stock(self):
        self.assertEqual(check.resolve({"a": [{"b": "x"}, {"b": "y"}]}, "a.1.b"), "y")
        self.assertIsNone(check.resolve({"a": []}, "a.0"))
        self.assertEqual(check.resolve([1], ""), [1])


if __name__ == "__main__":
    unittest.main()
