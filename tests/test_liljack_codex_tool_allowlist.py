"""Check Codex plugin exposure without logging unrelated config or credentials.

python3 tests/test_liljack_codex_tool_allowlist.py --config ~/.codex/config.toml
Default unittest/pytest mode uses fixtures only; --config explicitly audits a host.
"""
import argparse
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tomllib
import unittest

PLUGIN = "liljack@personal"
SERVER = "liljack"


def inventory(adapter):
    request = {"jsonrpc": "2.0", "id": 1, "method": "tools/list", "params": {}}
    run = subprocess.run(
        [sys.executable, str(adapter), "mcp"],
        input=json.dumps(request) + "\n", text=True, capture_output=True,
        env={**os.environ, "LILJACK_HARNESS": "codex", "LILJACK_AGENT": "codex"},
        timeout=15,
    )
    if run.returncode:
        raise ValueError("MCP inventory subprocess failed; output withheld")
    try:
        reply = json.loads(run.stdout)
        names = [item["name"] for item in reply["result"]["tools"]]
    except (ValueError, KeyError, TypeError):
        raise ValueError("Invalid MCP inventory; output withheld") from None
    if not names or len(names) != len(set(names)):
        raise ValueError("Empty or duplicate MCP inventory")
    return set(names)


def check(config, expected):
    plugin = config.get("plugins", {}).get(PLUGIN, {})
    server = plugin.get("mcp_servers", {}).get(SERVER, {})
    errors = []
    if plugin.get("enabled") is not True:
        errors.append("plugin not explicitly enabled")
    if server.get("enabled", True) is not True:
        errors.append("server disabled")
    allowed = server.get("enabled_tools")
    if not isinstance(allowed, list) or not all(isinstance(n, str) for n in allowed):
        errors.append("explicit enabled_tools list missing or invalid")
    else:
        if len(allowed) != len(set(allowed)):
            errors.append("duplicate enabled_tools entries")
        if not expected.issubset(allowed):
            errors.append("advertised tools missing from enabled_tools")
    denied = server.get("disabled_tools", [])
    if not isinstance(denied, list) or not all(isinstance(n, str) for n in denied):
        errors.append("disabled_tools list invalid")
    elif expected.intersection(denied):
        errors.append("advertised tools disabled")
    return errors


class AllowlistTests(unittest.TestCase):
    def setUp(self):
        self.names = {"task_update", "board_update", "task_assign"}
        self.config = {"plugins": {PLUGIN: {"enabled": True, "mcp_servers": {
            SERVER: {"enabled_tools": sorted(self.names)}}}}}

    def server(self):
        return self.config["plugins"][PLUGIN]["mcp_servers"][SERVER]

    def test_complete(self):
        self.assertEqual(check(self.config, self.names), [])

    def test_approval_overrides_are_not_exposure_list(self):
        self.server().pop("enabled_tools")
        self.server()["tools"] = {n: {"approval_mode": "approve"} for n in self.names}
        self.assertTrue(check(self.config, self.names))

    def test_each_missing_tool(self):
        for name in self.names:
            c = copy.deepcopy(self.config)
            c["plugins"][PLUGIN]["mcp_servers"][SERVER]["enabled_tools"].remove(name)
            self.assertTrue(check(c, self.names))

    def test_disabled_wins(self):
        self.server()["disabled_tools"] = ["task_update"]
        self.assertTrue(check(self.config, self.names))

    def test_disabled_plugin_or_server(self):
        self.server()["enabled"] = False
        self.assertTrue(check(self.config, self.names))
        self.server()["enabled"] = True
        self.config["plugins"][PLUGIN]["enabled"] = False
        self.assertTrue(check(self.config, self.names))

    def test_diagnostics_never_echo_config_values(self):
        self.server()["enabled_tools"] = ["fixture-private-value"]
        self.assertNotIn("fixture-private-value", str(check(self.config, self.names)))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path)
    parser.add_argument("--adapter", type=Path, default=Path(__file__).resolve().parents[1]
                        / "toolbox" / "liljack_codex.py")
    args = parser.parse_args()
    if args.config:
        try:
            names = inventory(args.adapter)
            errors = check(tomllib.loads(args.config.read_text()), names)
        except (OSError, ValueError, subprocess.TimeoutExpired):
            sys.exit("FAIL: audit input unavailable or invalid; contents withheld")
        print("Advertised tools: " + ", ".join(sorted(names)))
        for error in errors:
            print("FAIL: " + error)
        if errors:
            sys.exit(1)
        print(f"PASS: all {len(names)} advertised tools explicitly enabled, none disabled")
    else:
        unittest.main(argv=[sys.argv[0]])
