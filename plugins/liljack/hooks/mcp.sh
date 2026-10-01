#!/usr/bin/env bash
# stdio MCP server — same resolution as run.sh
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="${LILJACK_TOOLBOX_ROOT_ROOT:-}"
[ -z "$root" ] && [ -f "$here/../../../toolbox/liljack_mcp.py" ] && root="$(cd "$here/../../.." && pwd)"
[ -z "$root" ] && root="${LILJACK_REPO:-$PWD}"
exec python3 "$root/toolbox/liljack_mcp.py"
