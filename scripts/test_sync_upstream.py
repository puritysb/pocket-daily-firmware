"""Exercise sync safety with real temporary repositories, without network access."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).with_name("sync-upstream.sh").resolve()


class SyncUpstreamTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.upstream = self.root / "upstream"
        self.work = self.root / "work"
        self.env = dict(os.environ, GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull,
                        GIT_AUTHOR_NAME="Test", GIT_AUTHOR_EMAIL="test@example.invalid",
                        GIT_COMMITTER_NAME="Test", GIT_COMMITTER_EMAIL="test@example.invalid")
        self.git(self.root, "init", "-b", "master", str(self.upstream))
        self.commit(self.upstream, "reader.txt", "base\n")
        self.base = self.git(self.upstream, "rev-parse", "HEAD").strip()
        self.git(self.root, "clone", str(self.upstream), str(self.work))
        self.git(self.work, "remote", "rename", "origin", "upstream")
        self.git(self.work, "branch", "main")
        self.git(self.work, "switch", "-c", "feature/integration")
        self.commit(self.work, "pocket.txt", "product\n")
        self.head = self.git(self.work, "rev-parse", "HEAD").strip()
        self.commit(self.upstream, "reader.txt", "upstream\n")
        self.target = self.git(self.upstream, "rev-parse", "HEAD").strip()

    def git(self, cwd, *args):
        return subprocess.check_output(["git", *args], cwd=cwd, env=self.env,
                                       text=True, stderr=subprocess.STDOUT)

    def commit(self, cwd, name, data):
        (cwd / name).write_text(data)
        self.git(cwd, "add", name)
        self.git(cwd, "commit", "-m", "test: fixture")

    def sync(self, *args):
        return subprocess.run(["bash", str(SCRIPT), *args], cwd=self.work, env=self.env,
                              text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    def assertUnchanged(self):
        self.assertEqual(self.git(self.work, "rev-parse", "HEAD").strip(), self.head)
        self.assertEqual(self.git(self.work, "rev-parse", "main").strip(), self.base)
        self.assertEqual(self.git(self.work, "branch", "--show-current").strip(), "feature/integration")

    def test_merge_pins_input_without_committing_or_moving_main(self):
        self.commit(self.upstream, "later.txt", "later release\n")
        result = self.sync("--branch", "feature/integration", "--ref", self.target)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertUnchanged()
        self.assertEqual(self.git(self.work, "rev-parse", "MERGE_HEAD").strip(), self.target)
        self.assertEqual((self.work / "reader.txt").read_text(), "upstream\n")
        self.assertEqual((self.work / "pocket.txt").read_text(), "product\n")
        self.assertFalse((self.work / "later.txt").exists())

    def test_check_does_not_change_dirty_files(self):
        (self.work / "reader.txt").write_text("user work\n")
        result = self.sync("--check")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertUnchanged()
        self.assertEqual((self.work / "reader.txt").read_text(), "user work\n")
        self.assertFalse((self.work / ".git/MERGE_HEAD").exists())

    def test_wrong_branch_does_not_checkout(self):
        result = self.sync("--branch", "main")
        self.assertNotEqual(result.returncode, 0)
        self.assertUnchanged()

    def test_dirty_worktree_is_rejected(self):
        (self.work / "user.txt").write_text("untracked work\n")
        self.assertNotEqual(self.sync().returncode, 0)
        self.assertUnchanged()
        self.assertEqual((self.work / "user.txt").read_text(), "untracked work\n")

    def test_unknown_arguments_are_rejected(self):
        for args in [("--branxh", "main"), ("--ref",), ("--check", "--dry-run")]:
            with self.subTest(args=args):
                self.assertEqual(self.sync(*args).returncode, 2)
        self.assertUnchanged()

    def test_nonstable_ref_is_rejected(self):
        result = self.sync("--ref", self.head)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not on the fetched upstream stable line", result.stdout)
        self.assertUnchanged()

    def test_dry_run_reports_conflicts_without_touching_index(self):
        self.commit(self.work, "reader.txt", "product reader\n")
        self.head = self.git(self.work, "rev-parse", "HEAD").strip()
        result = self.sync("--dry-run")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("reader.txt", result.stdout)
        self.assertUnchanged()
        self.assertEqual(self.git(self.work, "status", "--porcelain"), "")
        self.assertFalse((self.work / ".git/MERGE_HEAD").exists())

    def test_conflicted_merge_preserves_merge_state(self):
        self.commit(self.work, "reader.txt", "product reader\n")
        self.head = self.git(self.work, "rev-parse", "HEAD").strip()
        result = self.sync()
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertUnchanged()
        self.assertEqual(self.git(self.work, "rev-parse", "MERGE_HEAD").strip(), self.target)
        self.assertIn("<<<<<<<", (self.work / "reader.txt").read_text())


if __name__ == "__main__":
    unittest.main()
