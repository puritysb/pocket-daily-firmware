"""
PlatformIO pre-build script: inject git branch and short SHA into
CROSSPOINT_VERSION for the default (dev) environment.

Results in a version string like:  1.1.0-dev-feat-kosync-xpath-05c6cf8
Release environments are unaffected; they set CROSSPOINT_VERSION in the ini.
"""

import configparser
import hashlib
import os
import re
import subprocess
import sys


def warn(msg):
    print(f'WARNING [git_branch.py]: {msg}', file=sys.stderr)


def run_git_value(project_dir, args, label):
    try:
        value = subprocess.check_output(
            ['git', *args],
            text=True, stderr=subprocess.PIPE, cwd=project_dir
        ).strip()
        # Strip characters that would break a C string literal
        return ''.join(c for c in value if c not in '"\\')
    except FileNotFoundError:
        warn(f'git not found on PATH; {label} suffix will be "unknown"')
        return 'unknown'
    except subprocess.CalledProcessError as e:
        warn(
            f'git command failed (exit {e.returncode}): '
            f'{e.stderr.strip()}; {label} suffix will be "unknown"'
        )
        return 'unknown'
    except OSError as e:
        warn(
            f'OS error reading git {label}: {e}; '
            f'{label} suffix will be "unknown"'
        )
        return 'unknown'
    except Exception as e:  # pylint: disable=broad-exception-caught
        warn(
            f'Unexpected error reading git {label}: {e}; '
            f'{label} suffix will be "unknown"'
        )
        return 'unknown'


def get_git_branch(project_dir):
    branch = run_git_value(
        project_dir, ['rev-parse', '--abbrev-ref', 'HEAD'], 'branch'
    )
    # Detached HEAD has no branch name.
    if branch == 'HEAD':
        return 'detached'
    # Branch names commonly contain '/', which is awkward in OTA versions,
    # JSON logs, and staged filenames. Keep the identity but make it portable.
    return re.sub(r'[^A-Za-z0-9._-]+', '-', branch).strip('-') or 'unknown'


def get_git_short_sha(project_dir):
    return run_git_value(
        project_dir, ['rev-parse', '--short', 'HEAD'], 'short SHA'
    )


def get_worktree_fingerprint(project_dir):
    """Return a stable short digest for local changes, or None when clean.

    HEAD alone cannot distinguish two development images built from different
    uncommitted firmware revisions. Hash both tracked diffs and untracked,
    non-ignored files so the device status and crash report identify the exact
    source tree that produced them.
    """
    try:
        status = subprocess.check_output(
            ['git', 'status', '--porcelain=v1', '-z', '--untracked-files=all'],
            cwd=project_dir,
        )
        if not status:
            return None

        digest = hashlib.sha256()
        digest.update(subprocess.check_output(
            ['git', 'diff', '--binary', '--no-ext-diff', 'HEAD', '--', '.'],
            cwd=project_dir,
        ))
        untracked = subprocess.check_output(
            ['git', 'ls-files', '--others', '--exclude-standard', '-z'],
            cwd=project_dir,
        ).split(b'\0')
        for relative_bytes in sorted(path for path in untracked if path):
            relative = os.fsdecode(relative_bytes)
            digest.update(relative_bytes)
            digest.update(b'\0')
            path = os.path.join(project_dir, relative)
            if not os.path.isfile(path):
                continue
            with open(path, 'rb') as source:
                for chunk in iter(lambda: source.read(64 * 1024), b''):
                    digest.update(chunk)
        return digest.hexdigest()[:8]
    except (OSError, subprocess.CalledProcessError) as exc:
        warn(f'could not fingerprint dirty worktree: {exc}')
        return 'unknown'


def get_base_version(project_dir):
    ini_path = os.path.join(project_dir, 'platformio.ini')
    if not os.path.isfile(ini_path):
        warn(f'platformio.ini not found at {ini_path}; base version will be "0.0.0"')
        return '0.0.0'
    config = configparser.ConfigParser()
    config.read(ini_path)
    if not config.has_option('crosspoint', 'version'):
        warn('No [crosspoint] version in platformio.ini; base version will be "0.0.0"')
        return '0.0.0'
    return config.get('crosspoint', 'version')


def inject_version(env):
    # Only applies to the dev (default) environment; release envs set the
    # version via build_flags in platformio.ini and are unaffected.
    if env['PIOENV'] != 'default':
        return

    project_dir = env['PROJECT_DIR']
    base_version = get_base_version(project_dir)
    branch = get_git_branch(project_dir)
    short_sha = get_git_short_sha(project_dir)
    version_string = f'{base_version}-dev-{branch}-{short_sha}'
    worktree = get_worktree_fingerprint(project_dir)
    if worktree:
        version_string += f'-w{worktree}'

    env.Append(CPPDEFINES=[('CROSSPOINT_VERSION', f'\\"{version_string}\\"')])
    print(f'CrossPoint build version: {version_string}')


# PlatformIO/SCons entry point — Import and env are SCons builtins injected at runtime.
# When run directly with Python (e.g. for validation), a lightweight fake env is used
# so the git/version logic can be exercised without a full build.
try:
    Import('env')           # noqa: F821  # type: ignore[name-defined]
    inject_version(env)     # noqa: F821  # type: ignore[name-defined]
except NameError:
    class _Env(dict):
        def Append(self, **_): pass

    _project_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    inject_version(_Env({'PIOENV': 'default', 'PROJECT_DIR': _project_dir}))
