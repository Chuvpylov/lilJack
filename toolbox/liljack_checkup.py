#!/usr/bin/env python3
"""
liljack_checkup.py — is lilJack actually wired, fed and drawing?

One command, ~5s, stdlib only. Checks the things that silently rotted before
(2026-08-26 audit): hooks pointing at real files, the statusline rendering from
THIS session's JSON, the inject hook producing project context, the MCP server
answering, the caches the statusline reads being written by a live timer, the
pairs corpus keeping up with the archive, and no running claude carrying the
CLAUDE_CODE_CHILD_SESSION poison marker.

  python3 toolbox/liljack_checkup.py            # print report, exit 1 on failure
  python3 toolbox/liljack_checkup.py --signal   # also queue failures for the
                                                # next Claude prompt (liljack signal)
Run weekly by liljack-checkup.timer; listed in TODO.md as a recurring item.
"""
import json
import os
import pathlib
import re
import subprocess
import sys
import time

RING_HEADER = os.environ.get("LILJACK_ARCHIVE_AUTH_HEADER", "X-Ring-Token")  # auth header of the remote archive

ROOT    = pathlib.Path(__file__).resolve().parent.parent
TOOLBOX = ROOT / "toolbox"
HOME    = pathlib.Path.home()
CACHE   = HOME / ".cache" / "liljack"
SETTINGS = HOME / ".claude" / "settings.json"
ARCHIVE_INDEX = HOME / ".claude" / "session_archive" / "index.jsonl"
PAIRS   = ROOT / "orc_data" / "session_pairs.jsonl"
CACHE_MAX_AGE = 15 * 60          # liljack-sync.timer runs update every 5 min
PAIRS_LAG_DAYS = 14              # Stop hook mines pairs after every session

results = []   # (ok, name, detail)


def check(name, ok, detail=""):
    results.append((bool(ok), name, detail))
    print(("  ✓ " if ok else "  ✗ ") + name + (f" — {detail}" if detail else ""))


def _run(cmd, stdin=None, timeout=20, env=None):
    return subprocess.run(cmd, input=stdin, capture_output=True, text=True,
                          timeout=timeout, env=env)


def check_settings():
    try:
        cfg = json.loads(SETTINGS.read_text())
    except Exception as e:
        check("settings.json readable", False, str(e)); return
    hooks = cfg.get("hooks", {})
    for event in ("UserPromptSubmit", "PreToolUse", "Stop"):
        cmds = [h.get("command", "") for grp in hooks.get(event, []) for h in grp.get("hooks", [])]
        paths = [tok for c in cmds for tok in c.split() if tok.endswith(".py")]
        ok = bool(paths) and all(pathlib.Path(p).is_file() for p in paths)
        check(f"hook {event} → existing script", ok, " ".join(paths) or "none configured")
    sl = (cfg.get("statusLine") or {}).get("command", "")
    sl_path = next((t for t in sl.split() if t.endswith(".sh")), "")
    check("statusLine → existing script", sl_path and pathlib.Path(sl_path).is_file(), sl_path or "none")


def check_statusline():
    sid = os.environ.get("CLAUDE_CODE_SESSION_ID", "checkup-no-session")
    payload = json.dumps({"session_id": sid, "cwd": str(ROOT),
                          "workspace": {"current_dir": str(ROOT)}})
    t0 = time.time()
    try:
        r = _run(["bash", str(TOOLBOX / "liljack_status.sh")], stdin=payload, timeout=10)
    except subprocess.TimeoutExpired:
        check("statusline renders", False, "timed out (>10s)"); return
    dt = time.time() - t0
    line = r.stdout.strip()
    check("statusline renders", line.startswith("lilJack ") and r.returncode == 0,
          f"{dt:.2f}s · {line[:110]}")
    check("statusline < 1s", dt < 1.0, f"{dt:.2f}s")
    check("statusline has git + TODO segments", "⎇" in line and "☑" in line)
    # nosave must be the poison marker, never "file not there yet"
    marker = False
    try:
        env = dict(os.environ)
        env["CLAUDE_CODE_CHILD_SESSION"] = "1"
        p = subprocess.Popen(["sleep", "5"], env=env)
        env2 = dict(os.environ); env2["CLAUDE_PID"] = str(p.pid)
        r2 = _run(["bash", str(TOOLBOX / "liljack_status.sh")],
                  stdin=json.dumps({"session_id": "deadbeef-checkup", "cwd": str(ROOT)}),
                  timeout=10, env=env2)
        marker = "⚠nosave" in r2.stdout
        p.kill()
        env3 = {k: v for k, v in os.environ.items() if k != "CLAUDE_CODE_CHILD_SESSION"}
        p = subprocess.Popen(["sleep", "5"], env=env3)
        env4 = dict(os.environ); env4["CLAUDE_PID"] = str(p.pid)
        r3 = _run(["bash", str(TOOLBOX / "liljack_status.sh")],
                  stdin=json.dumps({"session_id": "deadbeef-checkup", "cwd": str(ROOT)}),
                  timeout=10, env=env4)
        clean = "⚠nosave" not in r3.stdout
        p.kill()
    except Exception as e:
        check("nosave = poison marker only", False, str(e)); return
    check("nosave = poison marker only", marker and clean,
          f"poisoned parent→{'nosave' if marker else 'MISSED'} · clean parent→{'silent' if clean else 'FALSE ALARM'}")


def check_inject():
    sys.path.insert(0, str(TOOLBOX))
    try:
        import liljack_inject as li
        r = li.build_context("checkup: does the inject hook produce project context",
                             str(ROOT), "checkup-fresh-session", persist=False)
        txt = r["text"]
        check("inject: TODO line with milestone", "[lilJack TODO]" in txt and "next (" in txt)
        check("inject: recent work from other sessions", "[lilJack recent work]" in txt)
        check("inject: no cheatsheet/priors where CLAUDE.md exists (they duplicate it)",
              "[lilJack project:" not in txt and "memory priors" not in txt)
        check("inject: turn-1 payload ≤ 2KB", len(txt) <= 2048, f"{len(txt)} bytes")
        # turn 2+ of the SAME session must dedup the stable block to ~0
        ctx = li._load_context()
        sid = ctx.get("session_id", "")
        live = os.environ.get("CLAUDE_CODE_SESSION_ID", "")
        if sid:
            # <2 keywords → the chain lookup is skipped, so this measures ONLY the stable-block dedup
            r2 = li.build_context("ok thanks", str(ROOT), sid, persist=False)
            check("inject: turn-2 dedup", r2["skipped_stable"] and len(r2["text"]) == 0,
                  f"{len(r2['text'])} bytes")
        elif live:
            # a live session whose dedup state is gone = something rewrote context.json
            check("inject: turn-2 dedup", False, "context.json has no session_id while a session is live")
        else:
            check("inject: turn-2 dedup", True, "no live session — not testable from a timer")
    except Exception as e:
        check("inject hook importable", False, repr(e))


def check_mcp():
    try:
        proc = subprocess.Popen([sys.executable, str(TOOLBOX / "liljack_mcp.py")],
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        def rpc(req):
            proc.stdin.write(json.dumps(req) + "\n"); proc.stdin.flush()
            return json.loads(proc.stdout.readline())
        r = rpc({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}})
        check("mcp: initialize", r["result"]["serverInfo"]["name"] == "liljack")
        r = rpc({"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}})
        listed = [t["name"] for t in r["result"]["tools"]]
        names = set(listed)
        expected = {"project_context", "project_todo", "search_chains", "project_history",
                    "send_message", "read_messages", "board_read", "board_update",
                    "task_list", "task_propose", "task_assign", "task_update"}
        check("mcp: 12 intended tools", names == expected and len(listed) == len(expected),
              ",".join(sorted(names)))
        r = rpc({"jsonrpc": "2.0", "id": 3, "method": "tools/call",
                 "params": {"name": "project_todo", "arguments": {"project": "demo"}}})
        body = "".join(c.get("text", "") for c in r["result"]["content"])
        check("mcp: project_todo answers from live TODO.md", "☑" in body and len(body) < 20000, f"{len(body)} bytes")
        proc.stdin.close(); proc.wait(timeout=5)
    except Exception as e:
        check("mcp server roundtrip", False, repr(e))


def check_feeders():
    now = time.time()
    for f in ("apollo_status", "session_health.json", "context.json"):
        p = CACHE / f
        age = now - p.stat().st_mtime if p.exists() else None
        check(f"cache {f} fresh (<{CACHE_MAX_AGE//60}m)", age is not None and age < CACHE_MAX_AGE,
              "missing" if age is None else f"{age/60:.1f} min old")
    r = _run(["systemctl", "--user", "is-active", "liljack-sync.timer"])
    check("liljack-sync.timer active", r.stdout.strip() == "active", r.stdout.strip())
    r = _run(["systemctl", "--user", "is-enabled", "liljack-checkup.timer"])
    check("liljack-checkup.timer enabled", r.stdout.strip() == "enabled", r.stdout.strip())
    # pairs corpus keeps up with the archive
    def newest_ts(path, key):
        best = ""
        for ln in path.read_text(errors="replace").splitlines():
            try: best = max(best, json.loads(ln).get(key, "") or "")
            except Exception: pass
        return best
    try:
        arch = newest_ts(ARCHIVE_INDEX, "first_ts"); pairs = newest_ts(PAIRS, "ts")
        lag = (time.mktime(time.strptime(arch[:19], "%Y-%m-%dT%H:%M:%S"))
               - time.mktime(time.strptime(pairs[:19], "%Y-%m-%dT%H:%M:%S"))) / 86400
        check(f"pairs corpus within {PAIRS_LAG_DAYS}d of archive", lag < PAIRS_LAG_DAYS,
              f"archive {arch[:10]} · pairs {pairs[:10]} · lag {lag:.0f}d")
    except Exception as e:
        check("pairs corpus freshness", False, repr(e))


def check_archive():
    """Every transcript archived, every archived session chained, nothing purgeable.

    The two real losses: ~91 sessions auto-purged at the 30-day default before
    the archiver existed (2026-03→04), and 2 sessions run with transcript
    saving off under an inherited CLAUDE_CODE_CHILD_SESSION (2026-08-04).
    """
    try:
        days = json.loads(SETTINGS.read_text()).get("cleanupPeriodDays", 30)
    except Exception:
        days = 30
    check("cleanupPeriodDays ≥ 365 (30 is the purge that lost 91 sessions)", days >= 365, f"{days}")
    projects = HOME / ".claude" / "projects"
    arch = HOME / ".claude" / "session_archive"
    tx = {p.stem: p for p in projects.glob("*/*.jsonl")}
    idx = {}
    try:
        for ln in ARCHIVE_INDEX.read_text().splitlines():
            try:
                d = json.loads(ln); idx[d["session_id"]] = d
            except Exception:
                pass
    except Exception as e:
        check("archive index readable", False, repr(e)); return
    stale = 60 * 60   # a live session may be a few minutes behind; an hour is a dead hook
    missing = [s for s, p in tx.items() if s not in idx and time.time() - p.stat().st_mtime > stale]
    check("every transcript on disk is archived", not missing,
          f"{len(tx)} on disk · {len(idx)} archived" + (f" · unarchived >1h: {','.join(s[:8] for s in missing[:5])}" if missing else ""))
    behind = [s for s, p in tx.items() if s in idx and p.stat().st_size > idx[s].get("size_bytes", 0)
              and time.time() - p.stat().st_mtime > stale]
    check("no archived copy is >1h behind its transcript", not behind,
          ",".join(s[:8] for s in behind[:5]) if behind else "")
    stored = {f.name.split(".")[0] for f in arch.glob("[0-9]*/[0-9]*/*/*.jsonl.gz")}
    nocopy = [s for s in idx if s not in stored]
    check("every index entry has its stored copy", not nocopy, ",".join(s[:8] for s in nocopy[:5]))
    chains = {f.name.split(".")[0] for f in (arch / "chains").rglob("*.chains.jsonl")}
    unchained = [s for s, d in idx.items() if s not in chains and d.get("message_count", 0) > 5]
    check("every archived session (>5 msgs) has chains", not unchained,
          (f"{len(unchained)} missing — run: python3 toolbox/liljack_chains.py --backfill") if unchained else f"{len(chains)} chained")
    try:
        # distinct sessions — a grown session is re-pushed and logged again (2026-09-02)
        synced = len({json.loads(l)["session_id"] for l in (arch / "apollo_sync.jsonl").open() if l.strip()})
    except Exception:
        synced = 0
    check("Apollo sync not falling behind (<20 pending)", len(idx) - synced < 20, f"{len(idx) - synced} pending")


def check_narration():
    """Share of chain steps that carry visible narration (`say`).

    Thinking text never reaches disk (Claude 5 family: display omitted, by
    design), so narration is the only reasoning-shaped training signal we
    control. Measured over the newest 15 chain files of this project.
    """
    cdir = HOME / ".claude" / "session_archive" / "chains" / "demo"
    files = sorted(cdir.glob("*.chains.jsonl"), key=lambda p: p.stat().st_mtime, reverse=True)[:15]
    steps = said = 0
    for f in files:
        for ln in f.read_text(errors="replace").splitlines():
            try:
                for s in json.loads(ln).get("steps", []):
                    steps += 1
                    said += bool((s.get("say") or "").strip())
            except Exception:
                pass
    rate = said / steps if steps else 0.0
    check("narration coverage ≥ 25% of steps (reasoning-shaped signal on disk)",
          steps == 0 or rate >= 0.25, f"{said}/{steps} steps = {100*rate:.0f}% over {len(files)} sessions")


def check_chain_io():
    """Chains carry the tool I/O training needs (schema 2026-09-02): full
    `input`, head+tail `result`, `uuid` back to the verbatim archive. Before
    that date a step kept 300 chars of output and the corpus renderer dropped
    even those — terminal output never reached a training doc."""
    cdir = HOME / ".claude" / "session_archive" / "chains" / "demo"
    files = sorted(cdir.glob("*.chains.jsonl"), key=lambda p: p.stat().st_mtime, reverse=True)[:15]
    steps = rich = 0
    for f in files:
        for ln in f.read_text(errors="replace").splitlines():
            try:
                for s in json.loads(ln).get("steps", []):
                    steps += 1
                    rich += ("input" in s and "result" in s and "uuid" in s)
            except Exception:
                pass
    check("chains carry full tool I/O (input/result/uuid on every step)",
          steps == 0 or rich == steps,
          f"{rich}/{steps} steps — run: python3 toolbox/liljack_chains.py --backfill --force")


def check_apollo_readback():
    """The newest archived session can be READ BACK from Apollo, byte-identical.

    Until 2026-09-02 the PUT route wrote into the CAS repo_store and no GET
    existed for it, so "synced to Apollo" was only ever verified by the push
    log. GET /api/repos/{repo}/files/{path} is the mirror of the PUT; this
    compares the served bytes to the local archive copy. Skipped (not failed)
    when Apollo is offline — that is the sync check's job.
    """
    import hashlib, urllib.request
    sys.path.insert(0, str(ROOT / "toolbox"))
    try:
        from session_archive import _apollo_url, _apollo_online, _apollo_ring_token, _load_index, ARCHIVE_ROOT, LILJACK_REPO
    except Exception as e:
        check("Apollo readback (import)", False, str(e)); return
    url = _apollo_url()
    if not url or not _apollo_online(url):
        print("  · Apollo readback skipped — offline"); return
    idx = _load_index()
    if not idx:
        return
    meta = max(idx.values(), key=lambda m: m.get("archived_at", ""))
    rel = meta.get("archive_path", "")
    local = ARCHIVE_ROOT / rel
    if not local.exists():
        check("Apollo readback", False, f"local copy missing: {rel}"); return
    try:
        req = urllib.request.Request(f"{url}/api/repos/{LILJACK_REPO}/files/{rel}",
                                     headers={RING_HEADER: _apollo_ring_token()})
        with urllib.request.urlopen(req, timeout=15) as r:
            remote = r.read()
        # gzip headers carry an mtime (bytes 4-7), and the local copy is
        # re-gzipped on every archive pass — compare the CONTENT, not the file.
        import gzip as _gz
        def _content(b):
            try:
                return _gz.decompress(b) if b[:2] == b"\x1f\x8b" else b
            except Exception:
                return b
        lb, rb = _content(local.read_bytes()), _content(remote)
        same = hashlib.sha256(lb).hexdigest() == hashlib.sha256(rb).hexdigest()
        check("Apollo readback — newest archived session served with identical content", same,
              f"{rel}: local {len(lb)} B vs remote {len(rb)} B (decompressed)")
    except Exception as e:
        check("Apollo readback — newest archived session served byte-identical", False,
              f"{rel}: {str(e)[:80]} — is the GET route deployed?")


def check_poison():
    bad = []
    for pid in _run(["pgrep", "-x", "claude"]).stdout.split():
        try:
            env = pathlib.Path(f"/proc/{pid}/environ").read_bytes().split(b"\0")
            if any(e.startswith(b"CLAUDE_CODE_CHILD_SESSION=") for e in env):
                bad.append(pid)
        except Exception:
            pass
    check("no running claude carries CLAUDE_CODE_CHILD_SESSION", not bad,
          ("pids " + ",".join(bad) + " — transcripts OFF for those") if bad else "")


def check_tests():
    for t in sorted(ROOT.glob("tests/test_liljack_*.py")):
        r = _run([sys.executable, str(t)], timeout=120)
        check(f"test {t.name}", r.returncode == 0, (r.stdout + r.stderr).strip().splitlines()[-1][:100] if (r.stdout + r.stderr).strip() else "")


def main():
    signal = "--signal" in sys.argv
    print("[lilJack checkup]", time.strftime("%Y-%m-%d %H:%M"))
    for fn in (check_settings, check_statusline, check_inject, check_mcp,
               check_feeders, check_archive, check_narration, check_chain_io, check_apollo_readback, check_poison, check_tests):
        try:
            fn()
        except Exception as e:
            check(fn.__name__, False, repr(e))
    failed = [(n, d) for ok, n, d in results if not ok]
    print(f"[lilJack checkup] {len(results) - len(failed)}/{len(results)} ok")
    if failed and signal:
        msg = "lilJack checkup FAILED (" + time.strftime("%Y-%m-%d") + "): " + \
              " · ".join(f"{n}{' — ' + d if d else ''}" for n, d in failed)[:1500] + \
              "\nrun: python3 toolbox/liljack_checkup.py"
        sys.path.insert(0, str(TOOLBOX))
        import liljack
        liljack.write_signal(msg)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
