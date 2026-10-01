#!/usr/bin/env bash
# Maintainers: cherry-pick lilJack commits onto the release branch.
# Usage: sync_public.sh <public-range> [internal-branch]
# Refuses any commit that trips tools/scan_private.sh. Never pushes.
# Files that exist only in the public tree (README, CONTRIBUTING, LICENSE,
# tools/, vendor/) are taken from the public side when they conflict.
set -euo pipefail
range=${1:?commit range, e.g. origin/main~3..origin/main}
branch=${2:-${LILJACK_RELEASE_BRANCH:-release}}
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
public_only='^(README\.md|CONTRIBUTING\.md|LICENSE|tools/|vendor/)'
mapfile -t commits < <(git rev-list --reverse "$range")
((${#commits[@]})) || { echo 'nothing to sync' >&2; exit 0; }
bash "$here/scan_private.sh" "${commits[@]}"
git checkout -q "$branch"
for c in "${commits[@]}"; do
    if ! git cherry-pick -x "$c" >/dev/null 2>&1; then
        mapfile -t conflicts < <(git diff --name-only --diff-filter=U)
        for f in "${conflicts[@]}"; do
            if [[ $f =~ $public_only ]]; then git checkout --theirs -- "$f"; git add -- "$f"
            else git cherry-pick --abort; echo "sync_public: conflict in $f (commit $c)" >&2; exit 1; fi
        done
        GIT_EDITOR=true git cherry-pick --continue >/dev/null
    fi
done
printf 'synced %d commit(s) into %s\n' "${#commits[@]}" "$branch"
