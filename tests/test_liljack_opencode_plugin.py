#!/usr/bin/env python3
"""plugins/opencode/liljack.ts — the opencode lifecycle plugin.

Three regressions this file exists to catch, each of which shipped once:

1. The plugin piped its payload through the `$` helper's `.stdin`. The type
   declares `stdin: WritableStream`; the `$` opencode actually hands a plugin
   does not expose it, so EVERY event threw "undefined is not an object
   (evaluating 'proc.stdin.getWriter')". Nothing was ever recorded.
2. The failure was reported with console.error — into a TUI, which scribbled
   over the rendered frame. Diagnostics belong in a file.
3. `chat.message` and `experimental.chat.system.transform` both asked the
   adapter for context, and the adapter's per-session digest cache served
   whichever arrived first. Injection was a coin flip.

The checks drive the real hooks through node, so they exercise the actual
subprocess path rather than a mock of it.
"""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PLUGIN = ROOT / "plugins" / "opencode" / "liljack.ts"

ok = True


def check(label, cond, detail=""):
    global ok
    print(("✓ " if cond else "✗ ") + label + (("  — " + str(detail)) if detail and not cond else ""))
    if not cond:
        ok = False


HARNESS = r"""
const mod = await import(PLUGIN_PATH)
const factory = mod.default
const out = { hooks: [], errors: [] }
if (typeof factory !== "function") {
  out.errors.push("default export is not a function")
  console.log(JSON.stringify(out)); process.exit(0)
}
const hooks = await factory({ directory: CWD, $: undefined })
out.hooks = Object.keys(hooks)
const sid = "ses_selftest_" + Date.now()
await hooks["chat.message"](
  { sessionID: sid, model: { providerID: "p", modelID: "m" } },
  { message: { role: "user" }, parts: [{ type: "text", text: "selftest" }] },
)
const first = { system: ["BASE"] }
await hooks["experimental.chat.system.transform"]({ sessionID: sid, model: {} }, first)
const second = { system: ["BASE"] }
await hooks["experimental.chat.system.transform"]({ sessionID: sid, model: {} }, second)
const env = { env: {} }
await hooks["shell.env"]({ cwd: CWD }, env)
// an assistant message must not be forwarded as a user prompt
const ignored = { system: ["BASE"] }
const sid2 = "ses_selftest_assistant_" + Date.now()
await hooks["chat.message"](
  { sessionID: sid2 }, { message: { role: "assistant" }, parts: [{ type: "text", text: "x" }] },
)
out.injected = first.system.length > 1 ? first.system[1] : ""
out.firstLen = first.system.length
out.secondLen = second.system.length
out.env = env.env
console.log(JSON.stringify(out))
"""


def run_harness():
    node = shutil.which("node")
    if not node:
        return None, "node not on PATH"
    src = (HARNESS
           .replace("PLUGIN_PATH", json.dumps(str(PLUGIN)))
           .replace("CWD", json.dumps(str(ROOT))))
    with tempfile.NamedTemporaryFile("w", suffix=".mjs", delete=False, dir="/tmp") as fh:
        fh.write(src)
        path = fh.name
    try:
        env = dict(os.environ, LILJACK_DEBUG="0")
        proc = subprocess.run([node, path], capture_output=True, text=True, timeout=120, env=env)
        if proc.returncode != 0:
            return None, proc.stderr.strip()[-400:]
        line = [l for l in proc.stdout.splitlines() if l.startswith("{")]
        if not line:
            return None, proc.stdout.strip()[-400:]
        return json.loads(line[-1]), ""
    finally:
        os.unlink(path)


src = PLUGIN.read_text() if PLUGIN.exists() else ""


def code_only(text):
    """Strip comments — the header documents the very defects we grep for."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return "\n".join(l for l in text.splitlines() if not l.lstrip().startswith("//"))


code = code_only(src)
check("plugin file exists", PLUGIN.exists(), PLUGIN)
check("never pipes through the $ helper's stdin", "proc.stdin.getWriter" not in code)
check("spawns the adapter itself", 'from "node:child_process"' in code)
check("no console output (a TUI is on the other end)", "console.error" not in code and "console.log" not in code)
check("adapter calls are bounded by a timeout", "TIMEOUT_MS" in code)
check("adapter path points at the opencode adapter", "liljack_opencode.py" in code)

data, err = run_harness()
if data is None:
    check("harness ran", False, err)
else:
    check("harness reported no load error", not data.get("errors"), data.get("errors"))
    for hook in ("chat.message", "experimental.chat.system.transform",
                 "tool.execute.before", "tool.execute.after", "event", "shell.env"):
        check("registers " + hook, hook in data.get("hooks", []))
    check("context is injected once", data.get("firstLen") == 2, data.get("firstLen"))
    check("context is not injected twice in a session", data.get("secondLen") == 1, data.get("secondLen"))
    inj = data.get("injected", "")
    check("injected block is the lilJack context", inj.startswith("[lilJack / opencode]"), inj[:60])
    check("injected block is non-trivial", len(inj) > 200, len(inj))
    check("shell.env stamps the agent", data.get("env", {}).get("LILJACK_AGENT") == "deepseek")
    check("shell.env stamps the harness", data.get("env", {}).get("LILJACK_HARNESS") == "opencode")

print(("PASS" if ok else "FAIL") + " — " + __file__.rsplit("/", 1)[-1])
sys.exit(0 if ok else 1)
