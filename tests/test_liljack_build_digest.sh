#!/usr/bin/env bash
# The cached-binary digest must not depend on the launching shell: a desktop
# launch (one locale) and an agent shell (another) used to hash the same
# sources in a different glob order, so every switch rebuilt the app.
set -euo pipefail
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build="$repo_dir/liljack_app/build.sh"
# Older build.sh ignores DIGEST_ONLY and prints the cached path; its suffix is the digest.
digest() { LILJACK_BUILD_DIGEST_ONLY=1 "$@" bash "$build" 2>/dev/null | tail -n1 | grep -oE '[0-9a-f]{20}$'; }
a=$(digest env LC_ALL=C)
b=$(digest env LC_ALL=en_US.UTF-8)
c=$(digest env -i HOME="$HOME" PATH=/usr/bin:/bin)
d=$(digest env LILJACK_HUI="$repo_dir/liljack_app/../../hui")
[[ $a =~ ^[0-9a-f]{20}$ ]] || { echo "FAIL: no digest printed ($a)" >&2; exit 1; }
for other in "$b" "$c" "$d"; do
    [[ $other == "$a" ]] || { echo "FAIL: digest differs by environment: $a vs $other" >&2; exit 1; }
done
# owkTerm's own build.sh (run directly by its desktop entry) has the same contract.
owk="$repo_dir/../owkTerm/build.sh"
if [[ -f $owk ]]; then
    odigest() { LILJACK_BUILD_DIGEST_ONLY=1 "$@" bash "$owk" owkterm 2>/dev/null | tail -n1 | grep -oE '[0-9a-f]{20}$'; }
    oa=$(odigest env LC_ALL=C); ob=$(odigest env LC_ALL=en_US.UTF-8)
    [[ $oa =~ ^[0-9a-f]{20}$ && $oa == "$ob" ]] || { echo "FAIL: owkTerm digest differs by locale: $oa vs $ob" >&2; exit 1; }
fi
echo 'PASS: build digest is environment-independent'
