"""Apply the reviewed storage SDK patch, refusing incompatible source changes."""
from pathlib import Path
import subprocess


def apply_storage_patch(project: Path) -> str:
    sdk = project / "open-x4-sdk"
    patch = project / "scripts" / "storage_sdk.patch"
    if not patch.is_file() or not (sdk / "libs/hardware/SDCardManager/include/SDCardManager.h").is_file():
        raise RuntimeError("Storage SDK or reviewed patch is missing")
    command = ["git", "apply", "--check"]
    if subprocess.run(command + ["--reverse", str(patch.resolve())], cwd=sdk,
                      capture_output=True).returncode == 0:
        return "already applied"
    checked = subprocess.run(command + [str(patch.resolve())], cwd=sdk,
                             capture_output=True, text=True)
    if checked.returncode:
        raise RuntimeError("Storage SDK patch needs review; source was not changed: " + checked.stderr)
    subprocess.run(["git", "apply", str(patch.resolve())], cwd=sdk, check=True)
    return "applied"


if "Import" in globals():
    Import("env")  # noqa: F821 - PlatformIO/SCons
    print("Storage SDK patch: " + apply_storage_patch(Path(env["PROJECT_DIR"])))  # noqa: F821
