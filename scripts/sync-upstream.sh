#!/usr/bin/env bash
# Fetch the stable CrossPoint line and prepare a reviewable merge. Never commit,
# switch branches, rebase, or push. --ref pins the integration input.
set -euo pipefail

usage() {
  echo "Usage: $0 [--branch EXISTING_BRANCH] [--ref STABLE_COMMIT] [--check | --dry-run]"
}
branch=""
ref="upstream/master"
mode="merge"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --branch|--ref)
      [[ $# -ge 2 && -n "$2" && "$2" != -* ]] || { usage >&2; exit 2; }
      if [[ "$1" == --branch ]]; then branch="$2"; else ref="$2"; fi
      shift 2 ;;
    --check|--dry-run)
      [[ "$mode" == merge ]] || { usage >&2; exit 2; }
      mode="${1#--}"; shift ;;
    --help|-h) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
  esac
done

git rev-parse --is-inside-work-tree >/dev/null
current=$(git branch --show-current)
branch="${branch:-$current}"
[[ -n "$branch" ]] || { echo "ERROR: select an existing integration branch; HEAD is detached." >&2; exit 1; }
git show-ref --verify --quiet "refs/heads/$branch" || { echo "ERROR: branch does not exist: $branch" >&2; exit 1; }
if [[ "$mode" == merge ]]; then
  [[ "$current" == "$branch" ]] || { echo "ERROR: check out $branch explicitly first; no branch was changed." >&2; exit 1; }
  [[ -z "$(git status --porcelain)" ]] || { echo "ERROR: preserve local changes before merging." >&2; exit 1; }
  if git rev-parse --verify -q MERGE_HEAD >/dev/null; then
    echo "ERROR: finish or abort the existing merge first." >&2; exit 1
  fi
fi

git remote get-url upstream >/dev/null
git fetch upstream --prune
target=$(git rev-parse --verify "${ref}^{commit}")
git merge-base --is-ancestor "$target" upstream/master || {
  echo "ERROR: $ref is not on the fetched upstream stable line." >&2; exit 1;
}
pending=$(git rev-list --count "$branch..$target")
echo "==> $branch: $pending upstream ancestry commits; target $target"
if [[ "$mode" == check ]]; then
  git log --oneline "$branch..$target"
  # Stable ancestry alone does not describe the development backlog. Report it
  # separately without allowing a prerelease ref into the merge path below.
  if git rev-parse --verify -q refs/remotes/upstream/develop >/dev/null; then
    development=$(git rev-parse refs/remotes/upstream/develop)
    development_pending=$(git rev-list --count "$branch..$development")
    echo "==> Development watch only: $development_pending commits absent from $branch ancestry; target $development"
    echo "==> This is not a missing-fixes count: downstream equivalents need source review."
  else
    echo "==> Development watch unavailable: upstream/develop was not fetched."
  fi
  echo "==> Check only; working files and local branches are unchanged. Integration remains stable-only."
  exit 0
fi
[[ "$pending" -gt 0 ]] || exit 0
if [[ "$mode" == dry-run ]]; then
  git merge-tree --write-tree --name-only "$branch" "$target"
  exit $?
fi

git merge --no-ff --no-commit "$target"
echo "==> Merge prepared, not committed. Resolve both reader and product contracts."
echo "==> Run default + gh_release builds, strict cppcheck, host and companion contract checks."
echo "==> Hardware acceptance remains required; commit/push only when authorized."
