#!/usr/bin/env bash
# Launch lilJack in the best available sixel-capable terminal, falling back
# gracefully. Prefers wezterm (full sixel) then mlterm then xterm (vt340), and
# finally the bare app on whatever terminal is already around it.
set -euo pipefail
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
launcher="$repo/liljack"

if command -v wezterm >/dev/null 2>&1; then
    exec wezterm start --class liljack -- "$launcher" "$@"
fi
if command -v mlterm >/dev/null 2>&1; then
    exec mlterm -e "$launcher" "$@"
fi
if command -v xterm >/dev/null 2>&1; then
    exec xterm -ti vt340 -bg '#060b14' -fg '#e1efff' -e "$launcher" "$@"
fi
exec "$launcher" "$@"
