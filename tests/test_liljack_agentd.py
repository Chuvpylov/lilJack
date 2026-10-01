#!/usr/bin/env python3
"""test_liljack_agentd.py — the agent runner: spawn, own, control.

Plain script, ✓/✗ per check, non-zero exit on failure — the house style.

⚠ ZERO API SPEND. No real `claude`/`codex` session that does work is ever
started: the pty backend is proven with `cat` and `python3 -c` fakes, and
the two CLIs are exercised only as `--version` under the runner. DeepSeek is
never called. Everything lives under a private cache root and a private
board file, so the live socket, board and transcripts are untouched.

⚠ The cache root is a SHORT path under /tmp: AF_UNIX caps the socket path at
~108 bytes and the session scratchpad path alone is longer than that.
"""
import json
import os
import signal
import socket
import stat
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SIBLING = ROOT.parent / "the trainer"
sys.path.insert(0, str(ROOT / "toolbox"))
sys.path.insert(0, str(SIBLING / "scripts"))

os.environ["CUDA_VISIBLE_DEVICES"] = ""
_base = "/tmp/claude-1000" if os.path.isdir("/tmp/claude-1000") else "/tmp"
TMP = Path(tempfile.mkdtemp(prefix="ljad-", dir=_base))
os.environ["LILJACK_CACHE"] = str(TMP)
os.environ["LILJACK_BOARD"] = str(TMP / "board.bmd")
# parent env deliberately DIRTY — the child must not see any of it
os.environ["DEEPSEEK_API_KEY"] = "sk-parent-deepseek-must-not-leak"
os.environ["ANTHROPIC_API_KEY"] = "sk-ant-parent-must-not-leak"
os.environ["OPENAI_API_KEY"] = "sk-openai-parent-must-not-leak"
os.environ["APOLLO_RING"] = "ring-parent-must-not-leak"

import liljack_agentd as D          # noqa: E402
import liljack_agents as A          # noqa: E402
import liljack_sessions as S        # noqa: E402

PASS = FAIL = 0


def check(cond, label, extra=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"  ✓ {label}")
    else:
        FAIL += 1
        print(f"  ✗ {label}" + (f" — {extra}" if extra else ""))


def wait_for(pred, timeout=5.0, step=0.05):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if pred():
            return True
        time.sleep(step)
    return pred()


def rq(req):
    # These lifecycle/control fixtures model several already-bound callers.
    # The real socket identity boundary is tested in test_liljack_agentd_identity.
    if req.get("op") in {"spawn", "stop", "kill", "send", "take_control", "give_control", "shutdown"}:
        return daemon.handle(req, identity=req.get("from", req.get("who", "operator")))
    return D.request(req, root=TMP)


def tail_texts(agent, n=200, direction=None):
    evs = rq({"op": "tail", "agent": agent, "n": n})["events"]
    return [e["text"] for e in evs if direction is None or e["dir"] == direction]


class EventTap:
    """A client on the `events` op, collecting every line on a thread."""
    def __init__(self):
        self.events, self._stop = [], False
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(str(D.socket_path(TMP)))
        self.fh = self.sock.makefile("rwb", buffering=0)
        self.fh.write(b'{"op":"events"}\n')
        first = json.loads(self.fh.readline())
        assert first.get("event") == "subscribed", first
        self.t = threading.Thread(target=self._run, daemon=True)
        self.t.start()

    def _run(self):
        try:
            for line in self.fh:
                if line.strip():
                    self.events.append(json.loads(line))
        except (OSError, ValueError):
            pass

    def close(self):
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.sock.close()

    def of(self, kind, agent=None):
        return [e for e in self.events if e.get("event") == kind
                and (agent is None or e.get("agent") == agent)]


# ══════════════════════════════════════════════════════════════════════════
print("[1] daemon + socket")
daemon = D.Daemon(root=TMP, grace=0.8).serve(block=False)
check(daemon.sock_path.exists(), "socket created")
check(stat.S_IMODE(os.stat(daemon.sock_path).st_mode) == 0o600,
      "socket mode is 0600", oct(stat.S_IMODE(os.stat(daemon.sock_path).st_mode)))
check(rq({"op": "ping"}).get("pong") is True, "ping answers")
try:
    D.Daemon(root=TMP).serve(block=False)
    check(False, "second daemon on the same socket is refused")
except RuntimeError as exc:
    check("another agentd" in str(exc), "second daemon on the same socket is refused")
check(rq({"op": "nope"}).get("ok") is False, "unknown op → ok:false")
lst = rq({"op": "list"})
check(lst["ok"] and [a["name"] for a in lst["agents"]] == ["claude", "codex", "deepseek"],
      "list names the three agents even when stopped")
check(all(a["state"] == "stopped" and a["pid"] is None for a in lst["agents"]),
      "all three start `stopped` with no pid")
check({a["name"]: a["role"] for a in lst["agents"]} ==
      {"claude": "main", "codex": "second", "deepseek": "untrusted"}, "roles labelled")
check(lst["holder"] == "operator" and rq({"op": "control"})["holder"] == "operator",
      "operator holds control at start")

# ══════════════════════════════════════════════════════════════════════════
print("[2] pty backend: spawn / send / tail / stop with a fake agent (cat)")
tap = EventTap()
r = rq({"op": "spawn", "agent": "codex", "args": {"cmd": ["cat"]}})
check(r["ok"] and isinstance(r["pid"], int) and r["backend"] == "pty", "spawn cat under pty", r)
pid_cat = r.get("pid")
check(wait_for(lambda: rq({"op": "list"})["agents"][1]["state"] == "running"), "state → running")
check(rq({"op": "spawn", "agent": "codex", "args": {"cmd": ["cat"]}}).get("refused"),
      "spawning an already-running agent is refused")
r = rq({"op": "send", "agent": "codex", "text": "hello from operator"})
check(r["ok"] and r["event"]["dir"] == "in" and r["event"]["text"] == "hello from operator",
      "send (operator) accepted and recorded as dir=in")
check(wait_for(lambda: "hello from operator" in tail_texts("codex", direction="out")),
      "cat echoed it back → tail shows dir=out line")
outs = tail_texts("codex", direction="out")
check(outs.count("hello from operator") == 1, "pty echo is OFF (line appears once as out)", outs)
tp = Path(rq({"op": "list"})["agents"][1]["transcript"])
lines = [json.loads(l) for l in tp.read_text().splitlines() if l.strip()]
check(tp.exists() and all(set(l) >= {"ts", "dir", "text"} for l in lines),
      "transcript JSONL on disk with ts/dir/text")
check(rq({"op": "tail", "agent": "codex", "n": 1})["events"][-1]["text"] == "hello from operator"
      and len(rq({"op": "tail", "agent": "codex", "n": 1})["events"]) == 1, "tail n=1 honoured")
r = rq({"op": "stop", "agent": "codex"})
check(r["ok"] and r["stopped"] and r["how"] == "SIGTERM" and r["exit"]["signal"] == signal.SIGTERM,
      "stop → SIGTERM sufficed", r)
check(wait_for(lambda: rq({"op": "list"})["agents"][1]["state"] == "exited"), "state → exited")
check(rq({"op": "list"})["agents"][1]["pid"] is None, "exited agent reports no pid")
check(rq({"op": "stop", "agent": "codex"})["stopped"] is False, "stop on an exited agent is a no-op")
check(rq({"op": "send", "agent": "codex", "text": "x"}).get("refused"), "send to exited agent refused")

# ══════════════════════════════════════════════════════════════════════════
print("[3] SIGKILL after grace — fake agent ignores SIGTERM")
STUBBORN = ["python3", "-u", "-c",
            "import signal,time,sys; signal.signal(signal.SIGTERM, signal.SIG_IGN); "
            "print('stubborn up', flush=True); time.sleep(60)"]
r = rq({"op": "spawn", "agent": "claude", "args": {"cmd": STUBBORN}})
check(r["ok"], "spawn stubborn agent", r)
check(wait_for(lambda: "stubborn up" in tail_texts("claude", direction="out")), "it printed")
t0 = time.monotonic()
r = rq({"op": "stop", "agent": "claude"})
dt = time.monotonic() - t0
check(r["ok"] and r["stopped"] and r["how"] == "SIGKILL", "stop escalated to SIGKILL", r)
check(r["exit"]["signal"] == signal.SIGKILL, "exit status says signal 9", r.get("exit"))
check(0.8 <= dt < 4.0, f"grace honoured before the kill ({dt:.2f}s, grace 0.8)")
r = rq({"op": "spawn", "agent": "claude", "args": {"cmd": STUBBORN}})
check(wait_for(lambda: "stubborn up" in tail_texts("claude", direction="out")[-3:]), "respawn after exit works")
r = rq({"op": "kill", "agent": "claude"})
check(r["ok"] and r["killed"] and r["exit"]["signal"] == signal.SIGKILL, "kill → SIGKILL immediately", r)

# ══════════════════════════════════════════════════════════════════════════
print("[4] env allowlist — the child sees none of the parent's secrets")
DUMP = ["python3", "-c", "import os,json; print(json.dumps(dict(os.environ)))"]
r = rq({"op": "spawn", "agent": "codex", "args": {"cmd": DUMP, "env": {"LJ_EXTRA": "1"}}})
check(r["ok"], "spawn env dumper")
check(wait_for(lambda: any(t.startswith("{") for t in tail_texts("codex", direction="out"))), "dumper printed")
env_line = [t for t in tail_texts("codex", direction="out") if t.startswith("{")][-1]
child_env = json.loads(env_line)
leaks = [k for k in child_env if any(s in k.upper() for s in ("DEEPSEEK", "ANTHROPIC", "OPENAI", "APOLLO"))]
check(not leaks, "no DEEPSEEK/ANTHROPIC/OPENAI/APOLLO variable in the child", leaks)
check(child_env.get("CUDA_VISIBLE_DEVICES") == "", "CUDA_VISIBLE_DEVICES is empty in the child")
check(child_env.get("LJ_EXTRA") == "1", "args.env extras reach the child")
check(set(child_env) <= set(A.minimal_env(extra={"LJ_EXTRA": "1"})) | {"LJ_EXTRA"} or
      set(child_env) - set(A.minimal_env()) == {"LJ_EXTRA"},
      "child env is exactly the allowlist (+extras)", sorted(set(child_env) - set(A.minimal_env())))
check("PATH" in child_env and "HOME" in child_env, "PATH and HOME are present")
env_probe = A.minimal_env()
check("DEEPSEEK_API_KEY" not in env_probe and "ANTHROPIC_API_KEY" not in env_probe,
      "minimal_env() itself carries no provider key")

# ══════════════════════════════════════════════════════════════════════════
print("[5] scrubbing at write — a planted credential literal never reaches disk")
import scrub_archives as SA                       # noqa: E402
PLANTED = "deadbeefcafef00d0123456789abcdef42424242"      # 40 hex → _LONG_HEX matches
cred = TMP / "planted_credential.json"
cred.write_text(json.dumps({"token": PLANTED}))
SA.CREDENTIAL_SOURCES.append(cred)
check(PLANTED in SA.collect_live_secrets(), "planted literal is a known live secret")
r = rq({"op": "spawn", "agent": "codex", "args": {"cmd": ["cat"]}})
check(r["ok"], "spawn cat for the scrub test")
r = rq({"op": "send", "agent": "codex", "text": f"key is {PLANTED} ok"})
check(r["ok"] and "[REDACTED]" in r["event"]["text"] and PLANTED not in r["event"]["text"],
      "dir=in event is scrubbed", r.get("event"))
check(wait_for(lambda: any("[REDACTED]" in t for t in tail_texts("codex", direction="out")[-3:])),
      "dir=out event is scrubbed")
raw = Path(rq({"op": "list"})["agents"][1]["transcript"]).read_text()
check(PLANTED not in raw, "the literal is nowhere in the transcript file")
check("[REDACTED]" in raw, "and [REDACTED] is")
# scrubber unavailable → OMIT, never raw
import liljack_chains as LC                       # noqa: E402
_orig = LC._redact_live_secrets
LC._redact_live_secrets = lambda text: (text, -1)
try:
    r = rq({"op": "send", "agent": "codex", "text": "plain text while scrubber is down"})
    check(r["event"]["text"] == A.OMITTED, "scrubber unavailable → text omitted, not written raw", r.get("event"))
finally:
    LC._redact_live_secrets = _orig
SA.CREDENTIAL_SOURCES.remove(cred)
rq({"op": "kill", "agent": "codex"})

# ══════════════════════════════════════════════════════════════════════════
print("[6] control: exactly one holder, roles, send gating")
check(rq({"op": "send", "agent": "codex", "text": "x", "from": "claude"}).get("refused"),
      "send from a non-holder agent is refused")
r = rq({"op": "take_control", "who": "claude"})
check(r.get("refused"), "only the operator may TAKE control", r)
r = rq({"op": "give_control", "from": "claude", "to": "codex"})
check(r.get("refused") and "does not hold" in r["error"], "give from a non-holder is refused", r)
check(rq({"op": "give_control", "from": "operator", "to": "claude"})["holder"] == "claude",
      "operator gives control to claude")
check(rq({"op": "list"})["agents"][0]["control"] is True, "list marks claude as control holder")
r = rq({"op": "spawn", "agent": "codex", "args": {"cmd": ["cat"]}})
r = rq({"op": "send", "agent": "codex", "text": "from the holder", "from": "claude"})
check(r["ok"], "send from the holder is accepted", r)
r = rq({"op": "send", "agent": "codex", "text": "from operator", "from": "operator"})
check(r["ok"], "operator may always send", r)
r = rq({"op": "send", "agent": "codex", "text": "from codex", "from": "codex"})
check(r.get("refused"), "a non-holder agent still cannot send")
# expectations come from the LIVE may_control (liljack_roles if importable, else the fallback)
roles_live = D.may_control("claude", "operator")      # the real roles module allows handing back to the operator
print(f"    roles source: {'liljack_roles' if roles_live else 'fallback table'}")
exp_cd = D.may_control("claude", "deepseek")
r = rq({"op": "give_control", "from": "claude", "to": "deepseek"})
check(bool(r.get("ok")) == exp_cd, f"claude → deepseek follows may_control ({exp_cd})", r)
if exp_cd:
    r = rq({"op": "give_control", "from": "deepseek", "to": "codex"})
    check(r.get("refused") and "roles forbid" in r["error"], "deepseek → codex forbidden by roles", r)
    check(rq({"op": "control"})["holder"] == "deepseek", "refused give leaves the holder unchanged")
check(rq({"op": "take_control", "who": "operator"})["holder"] == "operator", "operator takes it back")
check(rq({"op": "give_control", "from": "operator", "to": "codex"})["holder"] == "codex", "operator → codex")
exp_cdk = D.may_control("codex", "deepseek")
r = rq({"op": "give_control", "from": "codex", "to": "deepseek"})
check(bool(r.get("ok")) == exp_cdk, f"codex → deepseek follows may_control ({exp_cdk})", r)
rq({"op": "take_control", "who": "operator"}); rq({"op": "give_control", "from": "operator", "to": "codex"})
check(rq({"op": "give_control", "from": "codex", "to": "claude"})["holder"] == "claude", "codex → claude allowed")
check(rq({"op": "give_control", "from": "claude", "to": "nobody"}).get("refused"), "give to an unknown target refused")
rq({"op": "take_control", "who": "operator"})

print("    fallback table with liljack_roles made unimportable")
_saved_roles = sys.modules.pop("liljack_roles", None)
sys.modules["liljack_roles"] = None                    # import → ImportError
try:
    check(D.may_control("operator", "deepseek") and not D.may_control("claude", "operator"),
          "fallback: operator→any, claude→operator refused")
    check(D.may_control("claude", "codex") and D.may_control("claude", "deepseek"), "fallback: claude→codex|deepseek")
    check(D.may_control("codex", "claude") and not D.may_control("codex", "deepseek"), "fallback: codex→claude only")
    check(not any(D.may_control("deepseek", t) for t in ("operator", "claude", "codex")), "fallback: deepseek→nobody")
    rq({"op": "give_control", "from": "operator", "to": "claude"})
    r = rq({"op": "give_control", "from": "claude", "to": "operator"})
    check(r.get("refused") and "roles forbid" in r["error"], "daemon enforces the fallback table", r)
    check(rq({"op": "give_control", "from": "claude", "to": "deepseek"})["holder"] == "deepseek", "claude → deepseek under fallback")
    check(rq({"op": "give_control", "from": "deepseek", "to": "claude"}).get("refused"), "deepseek → claude refused under fallback")
finally:
    del sys.modules["liljack_roles"]
    if _saved_roles is not None:
        sys.modules["liljack_roles"] = _saved_roles
check(D.may_control("claude", "operator") == roles_live, "roles source restored")
rq({"op": "take_control", "who": "operator"})

print("    concurrent take/give from 6 threads")
n_ctl_before = len(tap.of("control"))
ops = [{"op": "take_control", "who": "operator"},
       {"op": "give_control", "from": "operator", "to": "claude"},
       {"op": "give_control", "from": "operator", "to": "codex"},
       {"op": "give_control", "from": "claude", "to": "codex"},
       {"op": "give_control", "from": "codex", "to": "claude"},
       {"op": "give_control", "from": "claude", "to": "deepseek"},
       {"op": "take_control", "who": "codex"}]
results, holders_seen, errs = [], [], []


def hammer(i):
    try:
        for k in range(25):
            r = rq(ops[(i + k) % len(ops)])
            results.append(r)
            h = rq({"op": "control"})["holder"]
            holders_seen.append(h)
    except Exception as exc:
        errs.append(repr(exc))


ths = [threading.Thread(target=hammer, args=(i,)) for i in range(6)]
[t.start() for t in ths]
[t.join(30) for t in ths]
check(not errs, "no client errors under concurrency", errs[:2])
VALID = {"operator", "claude", "codex", "deepseek"}
check(all(isinstance(h, str) and h in VALID for h in holders_seen),
      f"every `control` reply names exactly one valid holder ({len(holders_seen)} reads)")
check(all(r.get("ok") or r.get("refused") for r in results), "every op either succeeded or was refused, never errored")
time.sleep(0.5)
ctl = tap.of("control")[n_ctl_before:]
chain_ok = all(ctl[i]["previous"] == ctl[i - 1]["holder"] for i in range(1, len(ctl)))
check(chain_ok and len(ctl) > 0, f"control events form one unbroken chain ({len(ctl)} handovers)")
check(all(e["previous"] != e["holder"] for e in ctl), "no self-handover events")
check(all(e["by"] == e["previous"] or e["by"] == "operator" for e in ctl),
      "every handover was made by the previous holder or the operator")
rq({"op": "take_control", "who": "operator"})
rq({"op": "kill", "agent": "codex"})

# ══════════════════════════════════════════════════════════════════════════
print("[7] events stream — each state change delivered once")
n_spawn0, n_exit0 = len(tap.of("spawn", "claude")), len(tap.of("exit", "claude"))
r = rq({"op": "spawn", "agent": "claude", "args": {"cmd": ["python3", "-c", "print('one-shot')"]}})
check(r["ok"], "spawn a one-shot agent")
check(wait_for(lambda: len(tap.of("exit", "claude")) == n_exit0 + 1, 5), "exit event arrived")
time.sleep(0.4)
check(len(tap.of("spawn", "claude")) == n_spawn0 + 1, "exactly one spawn event",
      len(tap.of("spawn", "claude")) - n_spawn0)
check(len(tap.of("exit", "claude")) == n_exit0 + 1, "exactly one exit event")
ex = tap.of("exit", "claude")[-1]
check(ex.get("code") == 0 and ex.get("state") == "exited", "exit event carries code 0 + state", ex)
outs = [e for e in tap.of("output", "claude") if e.get("text") == "one-shot"]
check(len(outs) == 1, "the output line streamed exactly once", len(outs))
sp = tap.of("spawn", "claude")[-1]
check(sp.get("pid") == r["pid"] and sp.get("cmd") == r["cmd"], "spawn event carries pid + cmd")
tap2 = EventTap()
rq({"op": "give_control", "from": "operator", "to": "claude"})
time.sleep(0.3)
check(len(tap2.of("control")) == 1 and tap2.of("control")[0]["holder"] == "claude",
      "a second subscriber gets the control event once too")
rq({"op": "take_control", "who": "operator"})
tap2.close()

# ══════════════════════════════════════════════════════════════════════════
print("[8] tmux backend — skipped with a reason when tmux is absent")
if A.tmux_path() is None:
    r = rq({"op": "spawn", "agent": "codex", "args": {"cmd": ["cat"], "backend": "tmux"}})
    check(r.get("refused") and "tmux is not installed" in r["error"], "tmux spawn refused with reason", r)
    m = rq({"op": "matrix"})["matrix"]
    check(m["codex"]["tmux"].startswith("unavailable") and m["codex"]["pty"] == "yes",
          "backend matrix reports tmux unavailable, pty yes")
    try:
        A.make_agent("codex", "tmux", A.Transcript(TMP / "x.jsonl"))
        check(False, "make_agent('tmux') raises BackendUnavailable")
    except A.BackendUnavailable as exc:
        check("not installed" in str(exc), "make_agent('tmux') raises BackendUnavailable")
    check(rq({"op": "list"})["agents"][1]["state"] != "running", "refusal left nothing running")
else:
    print("  · tmux present — refusal path not exercised (tmux backend itself is UNVERIFIED)")
    check(True, "tmux present (skipped refusal checks)")

# ══════════════════════════════════════════════════════════════════════════
print("[9] sessions + board integration")
check("agentd" in S.SOURCES, "register_source('agentd') happened at daemon start")
rq({"op": "spawn", "agent": "codex", "args": {"cmd": ["cat"]}})
wait_for(lambda: rq({"op": "list"})["agents"][1]["state"] == "running")
src = D.session_source(root=TMP)
rows = {r["agent"]: r for r in src["rows"]}
check(src["ok"] and rows.get("codex", {}).get("event") == S.AVAILABLE_EVENT,
      "session_source lists the running agent as Available", src)
check(S.state_of(rows["codex"]["event"], 0) == "available", "…which maps to `available`, never `idle`")
check("claude" in rows and rows["claude"]["event"] == S.END_EVENT, "exited agents map to the end event")
snap = S.snapshot(sources={"agentd": D.session_source})
check(any(s.get("agent") == "codex" for s in snap.get("states", snap.get("sessions", [])))
      if isinstance(snap, dict) else True, "sessions.snapshot() consumes the source without error")
board = Path(os.environ["LILJACK_BOARD"]).read_text()
check("fact(board-note, control, operator" in board, "board note `control` written on handover", board[-300:])
rq({"op": "give_control", "from": "operator", "to": "codex"})
board = Path(os.environ["LILJACK_BOARD"]).read_text()
check("fact(board-note, control, codex" in board and "gave control" in board, "board note follows the holder")
rq({"op": "take_control", "who": "operator"})
st = json.loads((TMP / D.STATE_FILE).read_text())
check(st["holder"] == "operator" and st["agents"]["codex"]["state"] == "running", "state file mirrors the daemon")

# ══════════════════════════════════════════════════════════════════════════
print("[10] command lines for the real agents (no work done: --version only)")
c = A.command_for("claude", {"resume": "abc"})
check(c[:6] == ["claude", "-p", "--output-format", "stream-json", "--input-format", "stream-json"]
      and "--resume" in c, "claude argv: -p stream-json both ways, --resume")
c = A.command_for("codex", {"task": "say hi", "sandbox": "read-only"})
check(c[:3] == ["codex", "exec", "--json"] and c[-1] == "say hi" and "-s" in c, "codex argv: exec --json <task>")
check(A.command_for("codex", {"interactive": True}) == ["codex"], "codex interactive argv")
r = rq({"op": "spawn", "agent": "claude", "args": {"cmd": ["claude", "--version"]}})
check(r["ok"] and wait_for(lambda: any("Claude Code" in t for t in tail_texts("claude", direction="out")), 15),
      "claude --version ran under the runner")
r = rq({"op": "spawn", "agent": "codex", "args": {"cmd": ["codex", "--version"]}}) if \
    rq({"op": "kill", "agent": "codex"}) else None
check(r and r["ok"] and wait_for(lambda: any("codex-cli" in t for t in tail_texts("codex", direction="out")), 15),
      "codex --version ran under the runner")
r = rq({"op": "spawn", "agent": "deepseek"})
check((r["ok"] and r["pid"] is None and r["backend"] == "inproc") or (not r["ok"] and "deepseek" in r["error"]),
      "deepseek 'spawn' is in-process (pid None) or an honest credential refusal", r)
if r["ok"]:
    rq({"op": "stop", "agent": "deepseek"})
    check(rq({"op": "list"})["agents"][2]["state"] == "exited", "deepseek released without any call")

# ══════════════════════════════════════════════════════════════════════════
print("[11] negative control — a wrong expectation must FAIL")
_p, _f = PASS, FAIL
r = rq({"op": "send", "agent": "codex", "text": "x", "from": "claude"})
check(r.get("ok") is True, "(PLANTED WRONG) non-holder send is accepted")
neg_failed = FAIL == _f + 1
PASS, FAIL = _p, _f                                   # revert the plant
check(neg_failed, "negative control: the planted wrong expectation failed, then was reverted")

# ══════════════════════════════════════════════════════════════════════════
print("[12] shutdown")
tap.close()
daemon.shutdown()
check(not daemon.sock_path.exists(), "socket removed on shutdown")
check(wait_for(lambda: all(not a.alive() for a in daemon.agents.values()), 5), "every owned agent is dead")
try:
    os.kill(pid_cat, 0)
    check(False, "first cat pid is gone")
except ProcessLookupError:
    check(True, "first cat pid is gone")

print(f"\n{PASS} passed, {FAIL} failed  (root {TMP})")
sys.exit(1 if FAIL else 0)
