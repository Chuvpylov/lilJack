#!/usr/bin/env python3
"""
liljack_runner.py — lilJack drives Claude Code against TODO items.

Headless (default): tmux session `lj-<proj>-<itemid>` runs
  timeout <T> claude -p "<prompt>" --output-format stream-json --verbose --max-turns <N>
  → stream teed to ~/.cache/liljack/runs/<run_id>.stream.jsonl
  → on exit, this file's --finish updates ledger + TODO.md.
Interactive (-i): tmux session with a real `claude`; prompt via send-keys.
Attach any time:  tmux attach -t <run_id>

Usage:
  python3 toolbox/liljack_runner.py run [ref] [-i] [--permission-mode M]
  python3 toolbox/liljack_runner.py ps | tail <run_id> | stop <run_id> | attach <run_id>
  python3 toolbox/liljack_runner.py --finish <run_id>     # internal
"""
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from liljack_todo import find_todo, parse_todo, next_pending, check_item, append_note

CACHE  = Path.home() / ".cache" / "liljack"
RUNS   = CACHE / "runs"
LEDGER = RUNS / "ledger.jsonl"
CFG    = CACHE / "runner.json"
DEFAULTS = {"max_turns": 40, "timeout_min": 30, "permission_mode": ""}


def _cfg() -> dict:
    cfg = dict(DEFAULTS)
    try:
        cfg.update(json.loads(CFG.read_text()))
    except Exception:
        pass
    return cfg


def _append_ledger(ev: dict) -> dict:
    RUNS.mkdir(parents=True, exist_ok=True)
    ev = {"ts": time.strftime("%Y-%m-%dT%H:%M:%S"), **ev}
    with open(LEDGER, "a", encoding="utf-8") as f:
        f.write(json.dumps(ev, ensure_ascii=False) + "\n")
    return ev


def _ledger_latest() -> dict:
    latest: dict = {}
    if LEDGER.exists():
        for line in LEDGER.read_text(errors="replace").splitlines():
            try:
                ev = json.loads(line)
                latest[ev["run_id"]] = {**latest.get(ev["run_id"], {}), **ev}
            except Exception:
                pass
    return latest


def build_prompt(scope: str, item_text: str) -> str:
    return (f"Project scope: {scope}\n\n"
            f"Your single task from TODO.md: {item_text}\n\n"
            "Do ONLY this task — do not start other TODO items. Write tests where "
            "they apply and run them. When finished, end your final message with "
            "exactly one line: ITEM COMPLETE (verified) or ITEM INCOMPLETE (and why).")


def _tmux(*args) -> subprocess.CompletedProcess:
    return subprocess.run(["tmux", *args], capture_output=True, text=True, timeout=10)


def start_run(cwd, ref=None, interactive=False, permission_mode=None,
              max_turns=None, timeout_min=None, dry_run=False) -> dict:
    cfg = _cfg()
    max_turns   = max_turns   or cfg["max_turns"]
    timeout_min = timeout_min or cfg["timeout_min"]
    permission_mode = permission_mode if permission_mode is not None else cfg["permission_mode"]

    tp = find_todo(cwd)
    if not tp:
        raise SystemExit("[liljack run] no TODO.md — the rule: liljack todo init first")
    parsed = parse_todo(tp)
    pend = next_pending(parsed, 999)
    item = None
    if ref:
        item = next((i for i in pend
                     if i["id"] == ref or ref.lower() in i["text"].lower()), None)
    elif pend:
        item = pend[0]
    if not item:
        raise SystemExit(f"[liljack run] no pending item matches {ref!r}" if ref
                         else "[liljack run] nothing pending — TODO complete 🎉")

    proj   = Path(cwd).resolve().name
    run_id = f"lj-{proj}-{item['id']}"
    stream = RUNS / f"{run_id}.stream.jsonl"
    running = {r for r, ev in _ledger_latest().items() if ev.get("status") == "running"}
    if any(r.startswith(f"lj-{proj}-") for r in running):
        print(f"[liljack run] ⚠ another run is active for {proj} (ledger) — continuing anyway")

    ev = {"run_id": run_id, "project": proj, "item_id": item["id"],
          "item_text": item["text"], "mode": "interactive" if interactive else "headless",
          "cwd": str(Path(cwd).resolve()), "stream": str(stream),
          "status": "dry-run" if dry_run else "running"}
    if dry_run:
        return _append_ledger(ev)

    prompt = build_prompt(parsed["scope"], item["text"])

    if shutil.which("tmux") is None:
        if interactive:
            raise SystemExit("[liljack run] -i needs tmux: sudo pacman -S tmux")
        cmd = ["claude", "-p", prompt, "--output-format", "stream-json",
               "--verbose", "--max-turns", str(max_turns)]
        if permission_mode:
            cmd += ["--permission-mode", permission_mode]
        RUNS.mkdir(parents=True, exist_ok=True)
        with open(stream, "w") as out:
            subprocess.Popen(cmd, cwd=str(cwd), stdout=out, stderr=subprocess.STDOUT,
                             start_new_session=True)
        ev = _append_ledger(ev)
        print(f"[liljack run] ▶ {run_id} (no tmux — nohup fallback) · liljack tail {run_id}")
        return ev

    if interactive:
        _tmux("new-session", "-d", "-s", run_id, "-c", str(cwd), "claude")
        time.sleep(3)
        _tmux("send-keys", "-t", run_id, prompt, "Enter")
    else:
        pm = f"--permission-mode {shlex.quote(permission_mode)} " if permission_mode else ""
        inner = (f"cd {shlex.quote(str(cwd))} && timeout {timeout_min * 60} "
                 f"claude -p {shlex.quote(prompt)} --output-format stream-json "
                 f"--verbose --max-turns {max_turns} {pm}"
                 f"> {shlex.quote(str(stream))} 2>&1; "
                 f"python3 {shlex.quote(str(Path(__file__).resolve()))} --finish {run_id}")
        RUNS.mkdir(parents=True, exist_ok=True)
        r = _tmux("new-session", "-d", "-s", run_id, "bash", "-lc", inner)
        if r.returncode != 0:
            raise SystemExit(f"[liljack run] tmux failed: {r.stderr.strip()}")
    ev = _append_ledger(ev)
    print(f"[liljack run] ▶ {run_id} · {item['text'][:60]}\n"
          f"  watch: tmux attach -t {run_id} · liljack tail {run_id}")
    return ev


def parse_result(stream_path) -> dict:
    text, is_error = "", True
    try:
        for line in Path(stream_path).read_text(errors="replace").splitlines():
            try:
                rec = json.loads(line)
            except Exception:
                continue
            if rec.get("type") == "result":
                text = rec.get("result") or ""
                is_error = bool(rec.get("is_error"))
    except OSError:
        pass
    complete = (not is_error) and bool(re.search(r'^\s*ITEM COMPLETE\b', text, re.M))
    return {"text": text, "is_error": is_error, "complete": complete}


def finish_run(run_id: str) -> dict:
    ev = _ledger_latest().get(run_id)
    if not ev:
        return {"status": "unknown"}
    res = parse_result(ev["stream"])
    tp = find_todo(ev["cwd"])
    ts = time.strftime("%Y-%m-%d")
    summary = re.sub(r'\s+', ' ', res["text"]).strip()[:100]
    if res["complete"]:
        status = "done"
        if tp:
            check_item(tp, ev["item_id"], note=f"{ts} done by run {run_id} — {summary}")
    else:
        # empty stream text = claude never emitted a result record → killed by `timeout`
        status = "timeout" if not res["text"] else "failed"
        if tp:
            append_note(tp, ev["item_id"],
                        f"{ts} attempted {run_id} ({status}), not confirmed — {summary}"
                        if summary else f"{ts} attempted {run_id} ({status}), not confirmed")
    return _append_ledger({**ev, "status": status})


def list_runs() -> list:
    live = set()
    if shutil.which("tmux"):
        r = _tmux("ls", "-F", "#{session_name}")
        if r.returncode == 0:
            live = {s for s in r.stdout.splitlines() if s.startswith("lj-")}
    out = []
    for run_id, ev in sorted(_ledger_latest().items(), key=lambda kv: kv[1].get("ts", "")):
        out.append({**ev, "live": run_id in live})
    return out


def stop_run(run_id: str) -> bool:
    ok = False
    if shutil.which("tmux"):
        ok = _tmux("kill-session", "-t", run_id).returncode == 0
    ev = _ledger_latest().get(run_id)
    if ev:
        _append_ledger({**ev, "status": "stopped"})
    return ok


def tail(run_id: str, n: int = 20) -> str:
    ev = _ledger_latest().get(run_id)
    if not ev or not Path(ev["stream"]).exists():
        return f"[liljack] no stream for {run_id}"
    return "\n".join(Path(ev["stream"]).read_text(errors="replace").splitlines()[-n:])


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv:
        argv = ["ps"]
    if argv[0] == "--finish":
        ev = finish_run(argv[1])
        print(f"[liljack run] {argv[1]} → {ev.get('status')}")
        return 0
    cmd, rest = argv[0], argv[1:]
    if cmd == "run":
        interactive = "-i" in rest
        rest = [a for a in rest if a != "-i"]
        pm = None
        if "--permission-mode" in rest:
            i = rest.index("--permission-mode")
            pm = rest[i + 1]
            del rest[i:i + 2]
        start_run(os.getcwd(), ref=(rest[0] if rest else None),
                  interactive=interactive, permission_mode=pm)
    elif cmd == "ps":
        runs = list_runs()[-15:]
        if not runs:
            print("[liljack] no runs yet — liljack run")
        for r in runs:
            mark = "▶" if r.get("live") else {"done": "☑", "failed": "✗", "timeout": "⏱",
                                              "stopped": "⏹"}.get(r.get("status"), "·")
            print(f"  {mark} {r['run_id']:36s} {r.get('status','?'):8s} {r.get('item_text','')[:50]}")
    elif cmd == "tail":
        print(tail(rest[0]) if rest else "usage: liljack tail <run-id>")
    elif cmd == "stop":
        if rest:
            print(f"[liljack] stopped {rest[0]}" if stop_run(rest[0])
                  else f"[liljack] no live session {rest[0]} (ledger updated)")
        else:
            print("usage: liljack stop <run-id>")
    elif cmd == "attach":
        if not rest:
            print("usage: liljack attach <run-id>")
            return 1
        os.execvp("tmux", ["tmux", "attach", "-t", rest[0]])
    else:
        print(f"[liljack run] unknown: {cmd}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
