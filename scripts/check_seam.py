"""Check compile-time ownership edges; this does not verify device behavior."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
INCLUDES = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[">]', re.MULTILINE)
COMMON = {"CrossPointSettings.h", "components/UITheme.h", "network/FirmwareFlasher.h",
          "util/BookCacheUtils.h"}
# Existing narrow integration edges. Adding one requires an explicit review here.
EXCEPTIONS = {
    "src/pocket_daily/boot/ProductBoot.cpp": {
        "SilentRestart.h": "implements the declared product reboot targets",
        "fontIds.h": "installs the bounded boot UI font",
    },
    "src/pocket_daily/web/PocketEndpoints.cpp": {
        "RecentBooksStore.h": "reading sync and removal update the reader's recent list",
        "articles/ArticleStorage.h": "product article storage used by app routes",
    },
}


def violations(root=ROOT):
    errors = []
    source = root / "src"
    for area in ("web", "boot"):
        for path in sorted((source / "pocket_daily" / area).glob("*")):
            if path.suffix not in (".h", ".cpp"):
                continue
            relative = path.relative_to(root).as_posix()
            allowed = COMMON | EXCEPTIONS.get(relative, {}).keys()
            for name in INCLUDES.findall(path.read_text()):
                candidates = (path.parent / name, source / name)
                for candidate in candidates:
                    candidate = candidate.resolve()
                    if not candidate.is_file() or not candidate.is_relative_to(source.resolve()):
                        continue
                    inherited = candidate.relative_to(source.resolve()).as_posix()
                    if not inherited.startswith("pocket_daily/") and inherited not in allowed:
                        errors.append(f"{relative}: unregistered inherited include {inherited}")
                    break
    for path in sorted((root / "lib").rglob("*")):
        if path.suffix not in (".h", ".cpp", ".hpp", ".c"):
            continue
        for name in INCLUDES.findall(path.read_text(errors="replace")):
            # Inspect spelled paths as well as resolvable relative paths.
            target = (path.parent / name).resolve()
            if "pocket_daily/" in name or target.is_relative_to((source / "pocket_daily").resolve()):
                errors.append(f"{path.relative_to(root)}: product dependency {name}")
    host = source / "network/CrossPointWebServer.h"
    pocket = [name for name in INCLUDES.findall(host.read_text()) if "pocket_daily/" in name]
    if pocket != ["pocket_daily/web/PocketWebServices.h"]:
        errors.append("CrossPointWebServer.h: expected the single PocketWebServices umbrella include")
    return errors


if __name__ == "__main__":
    problems = violations()
    for problem in problems:
        print(problem)
    if not problems:
        print("Pocket seam dependency checks passed")
    raise SystemExit(bool(problems))
