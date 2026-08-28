"""
PlatformIO post-build script: stage the built firmware.bin to a convenient
location so you can copy it to an SD card and install on-device at any time,
without needing a USB connection for every iteration.

After every successful `pio run`, copies the freshly built binary to:

  firmware/update.bin                              -- always latest (SD install name)
  firmware/pocket-daily-<version>-<env>-<sha>.bin  -- versioned archive (keeps last 5)
  firmware/LATEST_BUILD.txt                        -- build metadata + install steps

Optional: set the FIRMWARE_STAGING_DIR environment variable (or the
`firmware_staging_dir` custom project option in platformio.local.ini) to also
copy update.bin to a mounted SD card reader path automatically.

The staging directory is gitignored.
"""

import configparser
import datetime
import glob
import os
import shutil
import subprocess
import sys

STAGING_DIR_NAME = "firmware"
MAX_VERSIONED_COPIES = 5
LEARNING_PACK_RELATIVE = os.path.join("pocket-daily", "learning", "jp-n3-ko.pdl")
WORLD_FONT_RELATIVE = os.path.join(".fonts", "PocketSansWorld", "PocketSansWorld_12.cpfont")


def _run_git(project_dir, args):
    try:
        return subprocess.check_output(
            ["git", *args], text=True, stderr=subprocess.DEVNULL, cwd=project_dir
        ).strip()
    except Exception:
        return None


def get_git_info(project_dir):
    branch = _run_git(project_dir, ["rev-parse", "--abbrev-ref", "HEAD"]) or "unknown"
    sha = _run_git(project_dir, ["rev-parse", "--short", "HEAD"]) or "unknown"
    if branch == "HEAD":
        branch = "detached"
    return branch, sha


def get_base_version(project_dir):
    ini_path = os.path.join(project_dir, "platformio.ini")
    config = configparser.ConfigParser()
    config.read(ini_path)
    return config.get("crosspoint", "version", fallback="0.0.0")


def get_custom_staging_dir(env):
    """Return an extra copy destination if FIRMWARE_STAGING_DIR is set."""
    env_var = os.environ.get("FIRMWARE_STAGING_DIR")
    if env_var:
        return env_var
    project_dir = env.get("PROJECT_DIR", "")
    if project_dir:
        local_ini = os.path.join(project_dir, "platformio.local.ini")
        if os.path.isfile(local_ini):
            config = configparser.ConfigParser()
            config.read(local_ini)
            for section in config.sections():
                if config.has_option(section, "firmware_staging_dir"):
                    return config.get(section, "firmware_staging_dir").strip()
    return None


def prune_versioned_copies(staging_dir, prefix):
    """Keep only the most recent MAX_VERSIONED_COPIES versioned .bin files."""
    pattern = os.path.join(staging_dir, f"{prefix}*.bin")
    copies = sorted(glob.glob(pattern), key=os.path.getmtime, reverse=True)
    for old in copies[MAX_VERSIONED_COPIES:]:
        try:
            os.remove(old)
        except OSError:
            pass


def write_manifest(manifest_path, base_version, env_name, branch, sha, date_str, file_size, versioned_name,
                   learning_pack_size, world_font_size):
    lines = [
        "Pocket Daily Firmware Staging",
        "=============================",
        "",
        f"Version:      {base_version}",
        f"Environment:  {env_name}",
        f"Git branch:   {branch}",
        f"Git SHA:      {sha}",
        f"Built:        {date_str}",
        f"File size:    {file_size:,} bytes ({file_size / 1024:.0f} KB)",
        f"File:         {versioned_name}",
        f"Learning:     jp-n3-ko.pdl ({learning_pack_size:,} bytes)",
        f"World font:   PocketSansWorld_12.cpfont ({world_font_size:,} bytes)",
        "",
        "Installation (SD card)",
        "----------------------",
        "1. Copy update.bin to the root of your SD card.",
        "2. Hold UP + POWER to boot into recovery firmware mode.",
        "3. Pick update.bin from the file browser.",
        "4. Confirm and wait for the flash + automatic restart.",
        "5. Copy the contents of sd-card/ to the SD card root for offline study and automatic CJK fonts.",
        "",
    ]
    with open(manifest_path, "w") as f:
        f.write("\n".join(lines))


def stage(source, target, env):
    """PlatformIO post-action callback: copy the built .bin to the staging dir."""
    project_dir = env["PROJECT_DIR"]
    env_name = env["PIOENV"]
    firmware_bin = str(target[0])

    if not os.path.isfile(firmware_bin):
        print("stage_firmware: build artifact not found, skipping", file=sys.stderr)
        return

    staging_dir = os.path.join(project_dir, STAGING_DIR_NAME)
    os.makedirs(staging_dir, exist_ok=True)

    base_version = get_base_version(project_dir)
    branch, sha = get_git_info(project_dir)
    date_str = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    date_compact = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")

    prefix = f"pocket-daily-{base_version}-{env_name}"
    versioned_name = f"{prefix}-{sha}-{date_compact}.bin"
    versioned_path = os.path.join(staging_dir, versioned_name)
    update_path = os.path.join(staging_dir, "update.bin")

    shutil.copy2(firmware_bin, update_path)
    shutil.copy2(firmware_bin, versioned_path)
    prune_versioned_copies(staging_dir, prefix)

    # The complete study corpus belongs on SD, not in the already 83%-full OTA
    # application slot. Rebuild it from the reviewed source on every successful
    # firmware build so staged binaries and the companion SD tree cannot drift.
    learning_builder = os.path.join(project_dir, "scripts", "build-learning-pack.py")
    learning_source = os.path.join(project_dir, "learning", "content", "jp-n3-ko.json")
    learning_output = os.path.join(staging_dir, "sd-card", LEARNING_PACK_RELATIVE)
    os.makedirs(os.path.dirname(learning_output), exist_ok=True)
    subprocess.run(
        [sys.executable, learning_builder, "--source", learning_source, "--output", learning_output],
        check=True,
        cwd=project_dir,
    )
    learning_pack_size = os.path.getsize(learning_output)
    learning_manifest = os.path.splitext(learning_output)[0] + ".manifest.json"
    world_font_source = os.path.join(project_dir, "assets", "fonts", "PocketWorld", "PocketSansWorld_12.cpfont")
    world_font_output = os.path.join(staging_dir, "sd-card", WORLD_FONT_RELATIVE)
    if not os.path.isfile(world_font_source):
        raise FileNotFoundError(
            "automatic world font is missing; run scripts/build-pocket-fonts.py"
        )
    os.makedirs(os.path.dirname(world_font_output), exist_ok=True)
    shutil.copy2(world_font_source, world_font_output)
    world_font_size = os.path.getsize(world_font_output)

    file_size = os.path.getsize(update_path)
    manifest_path = os.path.join(staging_dir, "LATEST_BUILD.txt")
    write_manifest(manifest_path, base_version, env_name, branch, sha, date_str, file_size, versioned_name,
                   learning_pack_size, world_font_size)

    extra_dir = get_custom_staging_dir(env)
    extra_msg = ""
    if extra_dir:
        try:
            os.makedirs(extra_dir, exist_ok=True)
            shutil.copy2(update_path, os.path.join(extra_dir, "update.bin"))
            mounted_pack = os.path.join(extra_dir, LEARNING_PACK_RELATIVE)
            os.makedirs(os.path.dirname(mounted_pack), exist_ok=True)
            shutil.copy2(learning_output, mounted_pack)
            mounted_manifest = os.path.splitext(mounted_pack)[0] + ".manifest.json"
            shutil.copy2(learning_manifest, mounted_manifest)
            mounted_font = os.path.join(extra_dir, WORLD_FONT_RELATIVE)
            os.makedirs(os.path.dirname(mounted_font), exist_ok=True)
            shutil.copy2(world_font_output, mounted_font)
            extra_msg = f"\n  → also copied to {extra_dir}/"
        except OSError as e:
            print(f"stage_firmware: could not copy to {extra_dir}: {e}", file=sys.stderr)

    print(f"\nStaged firmware → {os.path.relpath(staging_dir, project_dir)}/")
    print(f"  update.bin ({file_size:,} bytes){extra_msg}")
    print(f"  sd-card/{LEARNING_PACK_RELATIVE} ({learning_pack_size:,} bytes)")
    print(f"  sd-card/{WORLD_FONT_RELATIVE} ({world_font_size:,} bytes)")
    print(f"  {versioned_name}")


# PlatformIO/SCons entry point.
try:
    Import("env")  # noqa: F821  -- provided by PlatformIO at script load
    env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", stage)
except NameError:
    pass
