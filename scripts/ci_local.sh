#!/usr/bin/env bash
# Run the checks of .github/workflows/ci.yml on this machine, in CI's order:
# clang-format 21 on changed C/C++ files, strict cppcheck, script and host
# tests, then the default and gh_release builds with zero compiler warnings.
#
#   scripts/ci_local.sh [--base <ref>] [--skip-build]
#
# --base defaults to the merge base with origin/main. clang-format 21 comes
# from $CLANG_FORMAT, clang-format-21 on PATH, or build/clang-format-21
# (python3 -m venv build/clang-format-21 &&
#  build/clang-format-21/bin/pip install 'clang-format==21.*'); local
# Homebrew clang-format 23 wraps differently from CI and is not accepted.
# Never run two PlatformIO builds at once: they corrupt ~/.platformio.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
BASE=""
BUILD=1
while [[ $# -gt 0 ]]; do
  case "$1" in
    --base) BASE="$2"; shift 2 ;;
    --skip-build) BUILD=0; shift ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done
[[ -n "$BASE" ]] || BASE="$(git merge-base HEAD origin/main)"
LOGS="build/ci-local"
mkdir -p "$LOGS"
step() { printf '\n== %s\n' "$1"; }

step "clang-format 21 (changed files since ${BASE:0:8})"
FORMAT="${CLANG_FORMAT:-}"
if [[ -z "$FORMAT" ]]; then
  if command -v clang-format-21 >/dev/null 2>&1; then FORMAT=clang-format-21
  elif [[ -x build/clang-format-21/bin/clang-format ]]; then FORMAT=build/clang-format-21/bin/clang-format
  fi
fi
if [[ -z "$FORMAT" ]] || ! "$FORMAT" --version | grep -q 'version 21\.'; then
  echo "clang-format 21 not found; see the header of this script" >&2
  exit 1
fi
CHANGED=()
while IFS= read -r file; do
  [[ -f "$file" ]] && CHANGED+=("$file")
done < <(git diff --name-only --diff-filter=ACMR "$BASE" -- '*.c' '*.cpp' '*.h' '*.hpp' |
  awk '!/^(lib\/EpdFont\/builtinFonts\/|lib\/Epub\/Epub\/hyphenation\/generated\/|lib\/uzlib\/|lib\/miniz\/third_party\/)/')
if [[ ${#CHANGED[@]} -gt 0 ]]; then
  "$FORMAT" -style=file --dry-run --Werror "${CHANGED[@]}"
fi
echo "${#CHANGED[@]} files formatted as CI expects"

step "cppcheck (strict)"
CPPCHECK="${CPPCHECK:-build/native-cppcheck/cppcheck}"
python3 -m unittest discover -s scripts -p test_check_firmware.py
python3 scripts/check_firmware.py --cppcheck "$CPPCHECK" >"$LOGS/cppcheck.log" 2>&1 ||
  { tail -40 "$LOGS/cppcheck.log"; exit 1; }
grep -E 'PASSED|FAILED' "$LOGS/cppcheck.log" | tail -1

step "integration boundaries and script tests"
python3 scripts/check_seam.py
[[ -f scripts/check_crosspoint_plugin.py ]] && python3 scripts/check_crosspoint_plugin.py
python3 -m unittest discover -s scripts -p 'test_*.py'

step "host tests"
cmake -S test -B build/host-tests -DCMAKE_BUILD_TYPE=Release >"$LOGS/cmake.log"
cmake --build build/host-tests >"$LOGS/host-build.log" 2>&1 || { tail -40 "$LOGS/host-build.log"; exit 1; }
ctest --test-dir build/host-tests --output-on-failure -j 8 | tail -3

if [[ "$BUILD" -eq 1 ]]; then
  for env in default gh_release; do
    step "build $env"
    ./scripts/pio.sh run -e "$env" >"$LOGS/build-$env.log" 2>&1 || { tail -40 "$LOGS/build-$env.log"; exit 1; }
    warnings="$(grep -c 'warning:' "$LOGS/build-$env.log" || true)"
    grep -E 'RAM:|Flash:' "$LOGS/build-$env.log" | head -2
    if [[ "$warnings" -ne 0 ]]; then
      grep 'warning:' "$LOGS/build-$env.log" | head -20
      echo "$env: $warnings compiler warnings" >&2
      exit 1
    fi
  done
fi

step "local CI passed"
