"""
PlatformIO pre-build script: keep the shared build cache (`build_cache_dir`)
bounded.

SCons never evicts its cache. Object files are worth caching, but the linked
image changes with every source edit and is never reused, while
firmware.elf alone is about 80 MB with debug information. Keep the link
outputs out of the cache and prune the oldest entries once the cache exceeds
CACHE_LIMIT_BYTES. A pruned entry only costs a recompilation.
"""

import os
import sys

CACHE_LIMIT_BYTES = 4 * 1024 ** 3
CACHE_TARGET_BYTES = 3 * 1024 ** 3
LINK_OUTPUT_SUFFIXES = ('.elf', '.bin', '.map')


def cache_entries(cache_dir):
    entries = []
    for root, _, files in os.walk(cache_dir):
        for name in files:
            # SCons keeps its signature database and config at the top level.
            if root == cache_dir:
                continue
            path = os.path.join(root, name)
            try:
                stat = os.stat(path)
            except OSError:
                continue
            entries.append((stat.st_mtime, stat.st_size, path))
    return entries


def prune(cache_dir, limit=CACHE_LIMIT_BYTES, target=CACHE_TARGET_BYTES):
    """Delete the oldest entries when the cache exceeds limit. Return bytes freed."""
    entries = cache_entries(cache_dir)
    total = sum(size for _, size, _ in entries)
    if total <= limit:
        return 0
    freed = 0
    for _, size, path in sorted(entries):
        if total - freed <= target:
            break
        try:
            os.remove(path)
        except OSError:
            continue
        freed += size
    return freed


def apply(env):
    build_dir = env.subst('$BUILD_DIR')
    # Pre-scripts run before the espressif32 builder renames "program".
    program = env.subst('$PROGNAME')
    if program == 'program':
        program = 'firmware'
    env.NoCache([os.path.join(build_dir, program + suffix) for suffix in LINK_OUTPUT_SUFFIXES])

    cache_dir = env.subst('$BUILD_CACHE_DIR')
    if not cache_dir:
        return
    if not os.path.isdir(cache_dir):
        return
    freed = prune(cache_dir)
    if freed:
        print(f'Build cache pruned: {freed / 1024 ** 2:.0f} MB of oldest entries removed')


if '--self-test' in sys.argv:
    import tempfile
    with tempfile.TemporaryDirectory() as directory:
        os.makedirs(os.path.join(directory, 'aa'))
        open(os.path.join(directory, 'config'), 'w').close()
        for index in range(4):
            path = os.path.join(directory, 'aa', str(index))
            with open(path, 'wb') as entry:
                entry.write(b'x' * 100)
            os.utime(path, (index, index))
        assert prune(directory, limit=1000, target=500) == 0
        assert prune(directory, limit=300, target=200) == 200
        assert sorted(os.listdir(os.path.join(directory, 'aa'))) == ['2', '3']
        assert os.path.exists(os.path.join(directory, 'config'))
    raise SystemExit(0)

Import('env')  # noqa: F821 -- provided by PlatformIO
apply(env)     # noqa: F821
