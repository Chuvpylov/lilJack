#!/usr/bin/env bash
# Deterministic Python proofs only; no live config, agents, or network tests.
set -euo pipefail
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd -- "$repo_dir"
python_test_dir=$(mktemp -d)
trap 'rm -rf -- "$python_test_dir"' EXIT
mkdir -m 700 "$python_test_dir/runtime" "$python_test_dir/tmp"
export SDL_VIDEODRIVER=dummy
# Suites act as many sessions; a tile-launched shell must not bind them to one.
unset LILJACK_WORKSPACE_SESSION
export XDG_RUNTIME_DIR="$python_test_dir/runtime"
export TMPDIR="$python_test_dir/tmp"
run_proof() {
    local label=$1
    shift
    if "$@"; then
        printf 'Python proof %s: PASS\n' "$label"
    else
        local result=$?
        printf 'Python proof %s: FAIL (exit %s)\n' "$label" "$result" >&2
        return "$result"
    fi
}
run_proof skill-install python3 tests/test_liljack_skill_install.py
run_proof codex-tool-allowlist python3 tests/test_liljack_codex_tool_allowlist.py
run_proof mcp-routing python3 tests/test_liljack_mcp.py
run_proof canvas-cli python3 tests/test_liljack_canvas_cli.py
printf 'All Python lilJack proofs passed (4/4).\n'
