#!/usr/bin/env bash
# Install the case-sensitive lilJack command without modifying shell profiles.
set -euo pipefail
app_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$app_dir/.." && pwd)
python3 "$app_dir/install_skills.py"
bin_dir=${LILJACK_BIN_DIR:-"$HOME/.local/bin"}
mkdir -p -- "$bin_dir"
target="$bin_dir/lilJack"
if [[ -e "$target" || -L "$target" ]]; then
    if [[ -L "$target" ]] || ! grep -q -F '# lilJack native workspace launcher' "$target"; then
        echo 'An unrelated lilJack command already exists; refusing to overwrite it.' >&2
        exit 1
    fi
fi
tmp="$target.$$.tmp"
trap 'rm -f -- "$tmp"' EXIT
{
    printf '#!/usr/bin/env bash\n# lilJack native workspace launcher\nset -euo pipefail\n'
    # Default project: LILJACK_PROJECT, else the repo itself.
    default_project=$repo_dir
    printf 'launcher=%q\nproject=%q\n' "$repo_dir/liljack" "${LILJACK_PROJECT:-$default_project}"
    printf 'case "${1:-}" in\n  room|session|owkterm|agent) exec "$launcher" "$@" ;;\n  *) exec "$launcher" --project "$project" "$@" ;;\nesac\n'
} > "$tmp"
chmod 755 "$tmp"
mv -- "$tmp" "$target"
printf 'Installed %s\n' "$target"
