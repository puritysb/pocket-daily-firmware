from pathlib import Path
import tempfile
import unittest

from check_seam import ROOT, violations


class SeamTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.write("src/network/CrossPointWebServer.h", '#include "pocket_daily/web/PocketWebServices.h"\n')
        self.write("src/pocket_daily/web/PocketWebServices.h", "#pragma once\n")
        self.write("src/CrossPointSettings.h", "#pragma once\n")
        self.write("src/activities/Activity.h", "#pragma once\n")

    def write(self, name, text):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)

    def test_current_tree(self):
        self.assertEqual(violations(ROOT), [])

    def test_allowed_host_snapshot_dependency(self):
        self.write("src/pocket_daily/web/Route.cpp", '#include "CrossPointSettings.h"\n')
        self.assertEqual(violations(self.root), [])

    def test_new_activity_dependency_is_rejected(self):
        self.write("src/pocket_daily/web/Route.cpp", '#include "activities/Activity.h"\n')
        self.assertIn("unregistered inherited include", " ".join(violations(self.root)))

    def test_relative_path_cannot_bypass_ownership(self):
        self.write("src/pocket_daily/web/Route.cpp", '#include "../../activities/Activity.h"\n')
        self.assertIn("unregistered inherited include", " ".join(violations(self.root)))

    def test_library_cannot_depend_on_product(self):
        self.write("lib/Reader/Reader.cpp", '#include "pocket_daily/Model.h"\n')
        self.assertIn("product dependency", " ".join(violations(self.root)))

    def test_host_cannot_add_another_product_include(self):
        self.write("src/network/CrossPointWebServer.h", '#include "pocket_daily/web/PocketWebServices.h"\n#include "pocket_daily/Model.h"\n')
        self.assertIn("single PocketWebServices", " ".join(violations(self.root)))


if __name__ == "__main__":
    unittest.main()
