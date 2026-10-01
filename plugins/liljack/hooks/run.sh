#!/usr/bin/env bash
# lilJack hook runner. Resolves the lilJack checkout that holds the toolbox:
#   1. $LILJACK_TOOLBOX_ROOT_ROOT if set
#   2. this plugin's own checkout (plugins/liljack → repo root) when run in place
#   3. $LILJACK_REPO, else the current directory
# Every hook is a thin call into toolbox/*.py so the plugin and the
# settings.json wiring (the pre-plugin form) run the SAME code.
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="${LILJACK_TOOLBOX_ROOT_ROOT:-}"
[ -z "$root" ] && [ -f "$here/../../../toolbox/liljack_inject.py" ] && root="$(cd "$here/../../.." && pwd)"
[ -z "$root" ] && root="${LILJACK_REPO:-$PWD}"
[ -f "$root/toolbox/liljack_inject.py" ] || exit 0     # no toolbox → silently inert
case "${1:-}" in
  inject)  exec python3 "$root/toolbox/liljack_inject.py" 2>/dev/null ;;
  pretool) exec python3 "$root/toolbox/liljack_pretool.py" 2>/dev/null ;;
  stop)    exec python3 "$root/toolbox/session_archive.py" --stop 2>/dev/null ;;
  *)       exit 0 ;;
esac
