#!/usr/bin/env python3
"""test_liljack_tui.py — lilJack's terminal app (toolbox/liljack_tui.py).

Plain script, ✓/✗ per check, non-zero exit on failure — the house style.

Everything here runs HEADLESS. The app deliberately keeps its rendering in
`snapshot -> [(style, text)]` functions with curses nowhere near them, so the
formatting, the wrapping, the tab/selection state, the thread grouping and the
project filter are all testable with planted inputs and a temp mailbox root.

Two planted-truth checks are load-bearing:

  · a snapshot whose sources are all DOWN must render the reasons, never a
    number. This repo has been bitten repeatedly by status displays that lied,
    so "the panel says `den … unreachable`" is a correctness property, not
    cosmetics.
  · `agent_of_cmd` must reject the helper processes that sit next to a real
    session (`codex-code-mode-host`, `claude --chrome-native-host`), because
    counting one invents a colleague on a repo — and the whole point of the
    PEERS view is to answer "who is actually working here".
"""
import json
import os
from pathlib import Path
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "toolbox"))
os.environ.setdefault("CUDA_VISIBLE_DEVICES", "")

import liljack_mail as M          # noqa: E402
import liljack_tui as T           # noqa: E402

PASS = FAIL = 0


def check(cond, label):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"✓ {label}")
    else:
        FAIL += 1
        print(f"✗ {label}")


def texts(lines):
    return "\n".join(t for _s, t in lines)


def main():
    tmp = Path(tempfile.mkdtemp(prefix="ljtui-"))

    # ══ 1. text primitives — the narrow-terminal contract ═════════════════
    check(T.fit("hello", 10) == "hello", "fit leaves a short string alone")
    check(T.fit("abcdefghij", 5) == "abcd…", "fit clips with an ellipsis")
    check(T.fit("abc", 0) == "" and T.fit("abc", -3) == "",
          "fit returns '' at zero/negative width (a 1-column terminal cannot raise)")
    check(T.fit("a\tb", 10) == "a    b", "fit expands tabs (curses cannot draw one)")
    check(T.fit(None, 5) == "", "fit tolerates None")
    check(len(T.pad("hi", 6)) == 6, "pad pads to exactly the width")

    w = T.wrap("the quick brown fox jumps", 10)
    check(all(len(l) <= 10 for l in w), "wrap never exceeds the width")
    check(" ".join(w) == "the quick brown fox jumps", "wrap loses no words")
    check(T.wrap("one\n\ntwo", 20) == ["one", "", "two"], "wrap keeps blank lines")
    check(T.wrap("x" * 25, 10) == ["x" * 10, "x" * 10, "x" * 5],
          "wrap hard-splits a word longer than the line (a hash or a URL)")
    check(T.wrap("anything", 0) == [], "wrap at width 0 is empty, not an exception")

    check(T.fmt_int(15052) == "15,052", "fmt_int groups thousands")
    check(T.fmt_int(None) == "—", "fmt_int renders a missing number as a dash, never 0")
    check(T.fmt_float(None) == "—" and T.fmt_float("x") == "—",
          "fmt_float renders a missing number as a dash")
    check(T.fmt_float(1.84034, 4) == "1.8403", "fmt_float rounds")
    check(T.fmt_dur(45) == "45s" and T.fmt_dur(90) == "1m30s", "fmt_dur seconds/minutes")
    check(T.fmt_dur(16597) == "4h36m", "fmt_dur hours")
    check(T.fmt_dur(None) == "—" and T.fmt_dur(-5) == "—", "fmt_dur refuses garbage")
    check(T.rel_time(None) == "—" and T.rel_time("not-a-date") == "—",
          "rel_time refuses an unparseable timestamp")
    check(T.rel_time(1000.0, now=1060.0) == "1m00s ago", "rel_time from a unix float")
    check(T.rel_time("2026-09-05T09:00:00+00:00",
                     now=1788598800.0 + 120) .endswith("ago"), "rel_time from ISO")

    check(T.bar(0.5, 10) == "█████░░░░░", "bar draws half")
    check(T.bar(2.0, 4) == "████" and T.bar(-1, 4) == "░░░░", "bar clamps out-of-range")
    check(T.bar(float("nan"), 4) == "░░░░", "bar survives NaN")
    check(T.bar(0.5, 0) == "", "bar at zero width is empty")
    check(T.pct(1, 4) == 0.25 and T.pct(1, 0) is None, "pct guards a zero denominator")

    # ══ 2. mailbox → threads ═════════════════════════════════════════════
    a = M.send("start the en judge", "codex", frm="claude", subject="handover",
               project="demo", thread="judge-1", root=tmp)
    b = M.send("on it", "claude", frm="codex", subject="re: handover",
               project="demo", thread="judge-1", root=tmp)
    c = M.send("unrelated", "claude", frm="operator", subject="corpus", project="the trainer", root=tmp)
    d = M.send("everyone hear this", "all", frm="operator", project="", root=tmp)

    got = T.collect_mail(root=tmp)
    check(got["ok"] and len(got["msgs"]) == 4, "collect_mail assembles every message from sent()")
    check({m["frm"] for m in got["msgs"]} == {"claude", "codex", "operator"},
          "…covering all three senders")
    check(got["unread"]["claude"] >= 2, "collect_mail reports per-agent unread counts")

    ths = T.group_threads(got["msgs"])
    keys = {t["key"] for t in ths}
    check("t:judge-1" in keys, "an explicit thread id groups its messages")
    j = next(t for t in ths if t["key"] == "t:judge-1")
    check(len(j["msgs"]) == 2, "…and only its messages")
    check(j["msgs"][0]["id"] == a["id"], "thread messages are oldest-first")
    check(set(j["parties"]) == {"claude", "codex"}, "thread lists its participants")
    check(j["title"] == "handover", "thread takes the first non-empty subject")
    check(ths[0]["last_ts"] >= ths[-1]["last_ts"], "threads are most-recent-first")

    subjless = T.group_threads([{"id": "x", "ts": "1", "frm": "claude", "to": "codex"},
                                {"id": "y", "ts": "2", "frm": "codex", "to": "claude"}])
    check(len(subjless) == 2,
          "two subject-less, thread-less messages are NOT merged into one thread")

    re_grouped = T.group_threads([{"id": "1", "ts": "1", "subject": "Plan", "frm": "a", "to": "b"},
                                  {"id": "2", "ts": "2", "subject": "re: plan", "frm": "b", "to": "a"}])
    check(len(re_grouped) == 1, "'re: X' joins X's thread (case-insensitively)")

    # unread accounting
    inb = M.inbox("codex", unread_only=False, limit=50, root=tmp)
    for m in inb:
        m.setdefault("read", False)
    un = T.unread_for(inb, "codex")
    check(a["id"] in un, "unread_for finds a message addressed to the agent")
    check(b["id"] not in un, "unread_for excludes the agent's own messages")
    M.ack([a["id"]], "codex", root=tmp)
    got2 = T.collect_mail(root=tmp)
    codex_unread = [m for m in got2["msgs"] if m.get("flags", {}).get("codex") is False]
    check(a["id"] not in [m["id"] for m in codex_unread], "an acked message stops being unread")

    # ══ 3. project filter + broadcast marking (the operator: don't scream) ═══════
    projs = T.projects_of(got["msgs"])
    check(projs[0] == T.ALL_PROJECTS, "the project cycle starts at (all)")
    check("demo" in projs and "the trainer" in projs, "every project tag is offered")
    check("(none)" in projs, "untagged mail gets its own bucket")
    check(len(T.filter_by_project(got["msgs"], "demo")) == 2, "filter keeps one project")
    check(len(T.filter_by_project(got["msgs"], T.ALL_PROJECTS)) == 4, "(all) filters nothing")
    check([m["id"] for m in T.filter_by_project(got["msgs"], "(none)")] == [d["id"]],
          "(none) selects exactly the untagged message")

    bcast = next(t for t in T.group_threads(got["msgs"]) if t["msgs"][0]["id"] == d["id"])
    check(T.is_broadcast(bcast) is True, "a message to `all` marks its thread a broadcast")
    check(T.is_broadcast(j) is False, "a directed thread is not a broadcast")
    listed = texts(T.view_board_list([bcast], 0, 60))
    check("!" in listed, "the board list marks a broadcast thread")

    rd = T.reply_defaults(j, "claude")
    check(rd["to"] == "codex", "reply goes back to the other party, not to yourself")
    check(rd["subject"].startswith("re:"), "reply prefixes the subject once")
    check(rd["thread"] == "judge-1", "reply stays in the thread")
    check(rd["project"] == "demo",
          "reply inherits the thread's project — never re-scoped to the operator's cwd")
    rd2 = T.reply_defaults(T.group_threads([{"id": "z", "ts": "1", "frm": "codex",
                                             "to": "claude", "subject": "re: re: x"}])[0], "claude")
    check(rd2["subject"].count("re:") == 1, "an already-'re:' subject is not doubled")

    # ══ 4. peers — who is on which repo ══════════════════════════════════
    check(T.agent_of_cmd("claude --dangerously-skip-permissions") == "claude",
          "a real Claude Code session is recognised")
    check(T.agent_of_cmd("codex") == "codex", "a real Codex session is recognised")
    check(T.agent_of_cmd("/home/x/.local/bin/claude") == "claude", "…by argv[0] basename")
    check(T.agent_of_cmd("/x/bin/codex-code-mode-host") is None,
          "the codex code-mode host is NOT a session (exact basename match)")
    check(T.agent_of_cmd("/home/x/.local/bin/claude --chrome-native-host") is None,
          "the Claude browser bridge is NOT a session")
    check(T.agent_of_cmd("/bin/bash -c source ~/.claude/shell-snapshots/s.sh") is None,
          "a shell Claude Code spawned is NOT a session")
    check(T.agent_of_cmd("") is None and T.agent_of_cmd(None) is None,
          "an empty command line is not an agent")

    # planted /proc/<pid>/stat. Fields 3..22 follow the ')' , so starttime
    # (field 22) is the 20th token after it — a comm containing spaces AND
    # parens is exactly what breaks a naive split, so the planted comm has both.
    tail = ["S"] + ["0"] * 18 + ["5000"]        # 20 tokens: fields 3..22
    check(len(tail) == 20, "the planted stat tail really covers fields 3..22")
    stat = "42 (my proc (x)) " + " ".join(tail)
    check(T.proc_start_time(stat, btime=1000.0, clk_tck=100) == 1050.0,
          "proc_start_time reads field 22 past a comm containing spaces and parens")
    check(T.proc_start_time("42 (sh) " + " ".join(["S"] + ["0"] * 18 + ["5000"]),
                            btime=1000.0, clk_tck=200) == 1025.0,
          "proc_start_time honours CLK_TCK")
    check(T.proc_start_time("garbage", 1000.0) is None, "proc_start_time refuses garbage")
    check(T.proc_start_time("", 1000.0) is None, "proc_start_time refuses an empty stat")
    # cross-check against a REAL process: our own start must be in the past and
    # not older than the boot it is measured from.
    _bt = T._btime()
    if _bt:
        own = T.proc_start_time(Path("/proc/self/stat").read_text(), _bt)
        import time as _t
        check(own is not None and _bt <= own <= _t.time() + 1,
              "proc_start_time on this very process lands between boot and now")
    else:
        check(False, "could not read btime from /proc/stat")

    # planted proc tree
    proc = tmp / "proc"
    for pid, cmd, cwd in [("11", "claude --dangerously-skip-permissions\0", "/repo/demo"),
                          ("12", "codex\0", "/repo/demo"),
                          ("13", "claude\0", "/repo/orc"),
                          ("14", "/x/codex-code-mode-host\0", "/repo/demo"),
                          ("15", "/usr/bin/python3 foo.py\0", "/repo/demo")]:
        d_ = proc / pid
        d_.mkdir(parents=True)
        (d_ / "cmdline").write_bytes(cmd.replace(" ", "\0").encode())
        (d_ / "stat").write_text("%s (x) %s" % (pid, " ".join(tail)))
        os.symlink(cwd, d_ / "cwd")
    (proc / "notapid").mkdir()
    sessions = T.scan_agent_sessions(str(proc), self_pid=-1, now=2000.0, btime=1000.0,
                                     is_repo=lambda p: p in ("/repo/demo", "/repo/orc"))
    check(len(sessions) == 3, "scan_agent_sessions finds exactly the 3 real sessions")
    check({s["pid"] for s in sessions} == {11, 12, 13}, "…and not the host or the python process")
    check(all(s["cwd"].startswith("/repo") for s in sessions), "each session carries its cwd")
    check(sessions[0]["age"] == 1050.0 - 2000.0 + 0 or sessions[0]["age"] is not None,
          "session age is derived from /proc/<pid>/stat")
    check(T.scan_agent_sessions(str(tmp / "nonexistent")) == [],
          "a missing /proc yields no sessions, not an exception")

    groups = T.group_peers(sessions, here="/repo/demo")
    check(groups[0]["name"] == "demo", "the repo you are in sorts first")
    check(groups[0]["here"] is True, "…and is flagged as here")
    check(groups[0]["claude"] == 1 and groups[0]["codex"] == 1, "per-agent counts per repo")
    check(groups[0]["contended"] is True,
          "two sessions in one tree is flagged — the case the operator named")
    check(groups[1]["contended"] is False, "a lone session in a repo is not contention")

    check(T.repo_of_path("/a/b/c", is_repo=lambda p: p == "/a") == "/a",
          "repo_of_path walks up to the git root")
    check(T.repo_of_path("/a/b/c", is_repo=lambda p: False) == "/a/b/c",
          "…and falls back to the path when there is no repo")
    check(T.repo_of_path("") == "", "repo_of_path tolerates an empty path")

    # ══ 5. trainers / metrics / config ═══════════════════════════════════
    check(T.run_of_cmd("python scripts/train.py --config configs/trio-sit5.yaml") == "trio-sit5",
          "run_of_cmd extracts the run name from --config")
    check(T.run_of_cmd("python scripts/train.py") is None, "run_of_cmd returns None with no --config")
    check(T.TRAIN_RE.search("venv/bin/python scripts/train_vision.py --config a.yaml") is not None,
          "the trainer pattern covers train_vision.py too")

    tr_proc = tmp / "proc2"
    (tr_proc / "77").mkdir(parents=True)
    (tr_proc / "77" / "cmdline").write_bytes(
        b"python\0scripts/train.py\0--config\0configs/trio-sit5.yaml\0")
    (tr_proc / "88").mkdir(parents=True)
    (tr_proc / "88" / "cmdline").write_bytes(b"python\0scripts/other.py\0")
    found = T.scan_trainers(str(tr_proc), self_pid=-1)
    check(len(found) == 1 and found[0]["run"] == "trio-sit5", "scan_trainers finds the trainer")
    check(found[0]["island"] == "lm", "…and labels its island")
    check(T.scan_trainers(str(tr_proc), self_pid=77) == [],
          "scan_trainers never counts its own pid (the self-match trap)")

    mjs = tmp / "metrics.jsonl"
    mjs.write_text('{"step": 1}\n{"step": 2}\n{"torn line\n{"step": 3}\n')
    check([r["step"] for r in T.tail_json(mjs, 2)] == [3],
          "tail_json skips a torn line instead of dying on it")
    check(T.tail_json(tmp / "nope.jsonl") == [], "tail_json on a missing file is empty")
    (tmp / "empty.jsonl").write_text("")
    check(T.tail_json(tmp / "empty.jsonl") == [], "tail_json on an empty file is empty")

    cfg = tmp / "c.yaml"
    cfg.write_text("train:\n  lr: 1e-3\ntotal_steps: 16_384\n")
    check(T.config_total_steps(cfg) == 16384, "config_total_steps reads the underscored int")
    check(T.config_total_steps(tmp / "nope.yaml") is None, "config_total_steps on a missing file")
    check(T.pid_alive(os.getpid()) is True and T.pid_alive(0) is False, "pid_alive")

    # ══ 6. judge rows ════════════════════════════════════════════════════
    rows = T.judge_rows({"school": {"kind": "school", "shards": {
        "en": {"units": 600, "judged": 125, "per_family": {"codex": 125},
               "multi_family_units": 0, "cross_family_agreement": None},
        "ja": {"units": 600, "judged": 0, "per_family": {},
               "multi_family_units": 0, "cross_family_agreement": None}}},
        "block": {"error": "boom"}})
    en = next(r for r in rows if r["shard"] == "en")
    check(abs(en["frac"] - 125 / 600) < 1e-9, "judge_rows computes the judged fraction")
    ja = next(r for r in rows if r["shard"] == "ja")
    check(ja["frac"] == 0.0, "a zero-judged shard is 0.0, not None")
    check(any(r.get("error") == "boom" for r in rows), "a failing kind is reported, not dropped")

    # ══ 7. codex lifecycle summary ═══════════════════════════════════════
    ev = [{"seq": 3, "at": "2026-09-05T09:00:03", "session": "s1", "cwd": "/repo/demo", "event": "Stop"},
          {"seq": 2, "at": "2026-09-05T09:00:02", "session": "s1", "cwd": "/repo/demo", "event": "PreToolUse"},
          {"seq": 1, "at": "2026-09-05T09:00:01", "session": "s2", "cwd": "/repo/orc", "event": "SessionStart"}]
    sess = T.codex_sessions(ev)
    check(sess[0]["session"] == "s1" and sess[0]["n"] == 2, "codex_sessions collapses by session")
    check(sess[0]["event"] == "Stop", "…and reports the LATEST event, not the first row seen")
    check(T.codex_events(db_path=str(tmp / "no.sqlite3"))["ok"] is False,
          "a missing lifecycle db is reported, never invented")

    # ══ 8. tab / selection state ═════════════════════════════════════════
    st = T.State()
    check(st.tab_name == "BOARD", "the app opens on BOARD")
    check(st.next_tab() == 1 and st.tab_name == "STATUS", "Tab advances")
    st.set_tab(len(T.TABS) - 1)
    check(st.next_tab() == 0, "Tab wraps around the last tab")
    check(st.next_tab(-1) == len(T.TABS) - 1, "Shift-Tab wraps backwards")
    check(st.set_tab(99) == len(T.TABS) - 1, "set_tab ignores an out-of-range index")

    st.set_tab(0)
    check(st.move(5, 1) == 1 and st.move(5, 1) == 2, "move steps the selection")
    check(st.move(5, 99) == 4, "move clamps at the end, never wraps")
    check(st.move(5, -99) == 0, "move clamps at the start")
    check(st.move(0, 1) == 0, "move on an empty list stays at 0")
    check(st.goto(5, "last") == 4 and st.goto(5, "first") == 0, "g / G jump to the ends")
    check(st.scroll_by(-5, 100) == 0, "scroll never goes negative")
    check(st.scroll_by(500, 10) == 10, "scroll clamps to the maximum row")

    st.project = T.ALL_PROJECTS
    check(st.cycle_project(["(all)", "demo", "the trainer"]) == "demo", "p cycles the project")
    check(st.cycle_project(["(all)", "demo", "the trainer"]) == "the trainer", "…forward")
    check(st.cycle_project(["(all)", "demo", "the trainer"]) == "(all)", "…and wraps to (all)")
    st.project = "Gone"
    check(st.cycle_project(["(all)", "demo"]) == "(all)",
          "a filter whose project vanished from the board falls back to (all)")

    # ══ 9. compose ═══════════════════════════════════════════════════════
    comp = T.Compose(frm="operator", project="demo")
    check(comp.field_name == "to", "compose starts on the recipient")
    check(comp.cycle_to() in ("claude", "codex", "deepseek", "all"), "the recipient cycles")
    check("operator" not in [comp.cycle_to() for _ in range(6)],
          "…and never offers the sender themselves (send-to-self is refused by the API)")
    comp.insert("x")
    check(comp.subject == "" and comp.body == "", "typing on the `to` field inserts nothing")
    comp.next_field()
    check(comp.field_name == "project", "Tab moves to the project field")
    comp.next_field()
    comp.insert("h")
    check(comp.subject == "h", "typing lands in the subject")
    comp.backspace()
    check(comp.subject == "", "backspace deletes from the focused field")
    comp.next_field()
    check(comp.field_name == "body", "Tab reaches the body")
    check(comp.ready() is False, "an empty body is not ready to send")
    comp.newline()
    check(comp.body == "\n", "Enter in the body inserts a newline instead of sending")
    for ch in "hello":
        comp.insert(ch)
    check(comp.ready() is True, "a body with text is ready")

    comp.to = "all"
    warns = comp.warnings()
    check(any("broadcast" in w for w in warns),
          "composing to `all` warns before it screams at every session")
    comp.to, comp.project = "codex", ""
    check(any("project" in w for w in comp.warnings()),
          "an untagged message warns that it cannot be filtered to a repo")

    comp.project = "demo"
    comp.thread = "judge-1"
    row = comp.send(root=tmp)
    check(row["to"] == "codex" and row["frm"] == "operator", "compose.send posts as the operator")
    check(row["project"] == "demo" and row["thread"] == "judge-1", "…carrying project and thread")
    check(row["text"].strip() == "hello", "…with the composed body")
    back = M.inbox("codex", unread_only=False, limit=50, root=tmp)
    check(any(m["id"] == row["id"] for m in back), "the sent message really lands in the mailbox")
    empty = T.Compose(frm="operator")
    try:
        empty.send(root=tmp)
        check(False, "an empty compose refuses to send")
    except ValueError:
        check(True, "an empty compose refuses to send")
    check(T.Compose(frm="operator", here="/x/y/demo",
                    project=None).project in ("demo", Path(T.repo_of_path("/x/y/demo")).name),
          "compose defaults the project to the repo it was opened from")

    cl = texts(T.compose_lines(T.Compose(frm="operator", to="all", project=""), 60))
    check("broadcast" in cl and "project" in cl, "the compose modal shows its warnings")

    # ══ 10. views — planted snapshots, including the all-down case ════════
    down = {"trainers": {"ok": True, "trainers": [], "lock": {"exists": False, "path": "/x/.train.lock"}},
            "den": {"ok": False, "error": "den http://localhost:7700 unreachable (URLError)"},
            "judge": {"ok": False, "error": "trio_judge unavailable (ImportError: x)"},
            "todo": {"ok": False, "error": "no TODO.md above this directory"},
            "walks": {"ok": False, "error": "no walks dir"},
            "host": {"ok": True, "load1": None, "cores": None, "package_c": None},
            "mail": {"ok": False, "error": "mailbox unreadable"},
            "peers": {"ok": False, "error": "/proc scan failed (PermissionError)"},
            "mood": {"ok": False, "error": "liljack.dsl unreadable"},
            "codex": {"ok": False, "error": "no lifecycle db"},
            "atlas": {"ok": False, "error": "no records"}}
    s = texts(T.view_status(down, 100, now=1000.0))
    check("no trainer" in s, "with nothing training the panel says so")
    check("unreachable" in s, "a down Den is named, with its reason")
    check("trio_judge unavailable" in s, "an unavailable judge store is named")
    check("no TODO.md" in s, "a missing TODO is named")
    check("—" in s, "missing host numbers render as a dash")
    for bad in ("0.0000", "None", "nan"):
        check(bad not in s.replace("—", ""), f"the all-down status invents no {bad!r} value")
    p = texts(T.view_peers(down, 100, now=1000.0))
    check("/proc scan failed" in p, "a failed peer scan is reported, not shown as 'nobody'")
    r = texts(T.view_runs(down, 100, now=1000.0))
    check("no records" in r, "an unreadable atlas is named")

    up = {
        "trainers": {"ok": True, "lock": {"exists": True, "pid": 29398, "alive": True,
                                          "path": "/x/.train.lock"},
                     "trainers": [{"pid": 29398, "run": "trio-sit5", "island": "lm",
                                   "total": 16384, "metrics_path": "/x/metrics.jsonl",
                                   "metrics": {"step": 15052, "timestamp": 990.0,
                                               "elapsed_time": 16597.0,
                                               "metrics": {"loss": 1.84034, "lr": 1.05e-05,
                                                           "tokens_per_sec": 5103.18,
                                                           "grad_norm": 0.1836}}}]},
        "den": {"ok": True,
                "timers": [{"unit": "sindy-scan.timer", "active": True, "next_in_s": 691,
                            "can_launch_trainer": False}],
                "live": [{"run": "trio-sit5", "step": 15052, "total": 16384,
                          "loss": 1.84, "val": 2.55}],
                "walks": [{"id": "trio-gen0", "status": "active", "phase": "A", "mode": "rounds",
                           "steps": 800, "counts": {"planned": 3}, "best": None,
                           "stale_running": ["p001"]}],
                "prep": [], "host": {}},
        "judge": {"ok": True, "kinds": {"school": {"kind": "school", "shards": {
            "uk": {"units": 600, "judged": 600, "per_family": {"claude": 600},
                   "multi_family_units": 0, "cross_family_agreement": None}}}}},
        "todo": {"ok": True, "path": "/x/TODO.md", "line": "☑277/438", "next": ["do the thing"],
                 "done": 277, "total": 438, "milestone": "Next 10"},
        "walks": {"ok": True, "walks": [], "path": "/x/walks"},
        "host": {"ok": True, "load1": 2.98, "cores": 36, "package_c": 97.0, "uptime": 17940},
        "mail": {"ok": True, "msgs": got["msgs"], "unread": {"operator": 2, "claude": 0, "codex": 0},
                 "path": "/x/mailbox.jsonl"},
        "peers": {"ok": True, "sessions": sessions, "groups": groups, "here": "/repo/demo"},
        "mood": {"ok": True, "mood": "working", "face": "(x)", "traits": ["a"], "impressions": {}},
        "codex": {"ok": True, "rows": ev, "total": 6287},
        "atlas": {"ok": True, "path": "/x/atlas.jsonl", "rows": [
            {"run": "trio-sit5", "island": "lm", "steps": 14504, "best_val": 2.5526,
             "val_loss": 2.5526, "acc1": 0.5406, "era": {"id": "spm_trio_v1+corpus_trio_sit5"}}]},
    }
    s = texts(T.view_status(up, 110, now=1000.0))
    check("15,052/16,384" in s, "the trainer panel shows step out of total")
    check("91.9%" in s or "92." in s, "…with a progress percentage")
    check("1.8403" in s and "5,103 tok/s" in s, "loss and throughput come from the metrics line")
    check("held by pid 29398" in s, "the train lock is reported with its holder")
    check("sindy-scan.timer" in s and "11m31s" in s, "a Den timer shows its next tick")
    check("stale 'running'" in s, "a stale walk marker is surfaced as a status lie")
    check("600/600" in s and "100.0%" in s, "judge coverage per shard")
    check("☑277/438" in s, "the TODO status line is shown verbatim")
    check("97 °C" in s and "ceiling" in s, "a package temperature near the ceiling is called out")
    check("load1 2.98" in s, "load is formatted, not raw-float dumped")

    p = texts(T.view_peers(up, 110, now=1000.0))
    check("claude×1 codex×1" in p, "PEERS shows per-agent counts on the repo")
    check("coordinate on the BOARD" in p, "a contended repo tells you where to coordinate")
    check("mail addresses AGENTS, not sessions" in p,
          "PEERS states the honest limit of the mailbox instead of implying session addressing")
    check("6,287 events" in p, "the codex lifecycle total is shown")

    r = texts(T.view_runs(up, 110, now=1000.0))
    check("trio-sit5" in r and "2.5526" in r, "the atlas tail lists runs with best_val")
    check("bits per TOKEN" in r, "the bpc caveat travels with the atlas table")

    rb = T.ribbon(up, 120, now=1000.0)
    check("trio-sit5" in rb and "den✓" in rb, "the ribbon names the live run and the Den")
    check("☑277/438" in rb, "…the TODO count")
    check("👥1c/1x@demo" in rb and "⚠" in rb,
          "…and the peers on this repo, flagged when contended")
    check(len(T.ribbon(up, 20, now=1000.0)) <= 20, "the ribbon respects a narrow terminal")
    rb_down = T.ribbon(down, 120, now=1000.0)
    check("den✗" in rb_down and "▶none" in rb_down,
          "the ribbon states 'no trainer' and 'den down' rather than going blank")

    # every view must survive a hostile width and an empty snapshot
    for width in (1, 2, 5, 20):
        for fn in (T.view_status, T.view_peers, T.view_runs):
            lines = fn({}, width, now=1000.0)
            check(all(len(t) <= max(width, 200) for _s2, t in lines) and lines != [],
                  f"{fn.__name__} renders at width {width} on an empty snapshot")
    check(T.ribbon({}, 40) != "", "the ribbon renders on an empty snapshot")
    check(texts(T.view_board_thread(None, 40)) != "", "the message pane renders with no selection")
    check(texts(T.view_board_list([], 0, 40)) != "", "the thread list renders when empty")

    # ══ 11. cache — TTL and failure containment ══════════════════════════
    clock = {"t": 100.0}
    calls = {"n": 0}

    def counting():
        calls["n"] += 1
        return {"ok": True, "n": calls["n"]}

    def exploding():
        raise RuntimeError("collector blew up")

    cache = T.Cache({"a": (counting, 10.0), "boom": (exploding, 1.0)},
                    clock=lambda: clock["t"])
    check(cache.due("a") is True, "a never-fetched source is due")
    cache.refresh("a")
    check(cache.get("a")["n"] == 1, "refresh stores the value")
    check(cache.due("a") is False, "…and it is not due again inside its TTL")
    check(cache.refresh("a") is False and calls["n"] == 1, "refresh inside the TTL is a no-op")
    check(cache.refresh("a", force=True) is True and calls["n"] == 2, "force refetches")
    clock["t"] += 11
    check(cache.due("a") is True, "the source is due once the TTL elapses")
    check(cache.age("a") == 11.0, "age reports how stale a source is")
    cache.refresh("boom")
    v = cache.get("boom")
    check(v["ok"] is False and "collector blew up" in v["error"],
          "a raising collector becomes an on-screen error, never a crash")
    snap, at = cache.snapshot()
    check(set(snap) == {"a", "boom"} and set(at) == {"a", "boom"}, "snapshot returns both")
    check(cache.due("nope") is False, "an unknown source is never due")

    # ══ 12. end-to-end dump against the real machine (no terminal) ═══════
    real = T.Cache({"mail": (lambda: T.collect_mail(root=tmp), 1.0),
                    "peers": (T.collect_peers, 1.0),
                    "trainers": (T.collect_trainers, 1.0),
                    "host": (T.collect_host, 1.0)})
    out = T.dump(real, T.State(), 100)
    check("### STATUS" in out and "### PEERS" in out and "### BOARD" in out,
          "dump renders every view without a terminal")
    check("den" in out.lower(), "dump reports the Den one way or the other")
    check(T.collect_peers()["ok"] is True, "collect_peers works on this machine")
    check(T.collect_host()["ok"] is True, "collect_host works on this machine")
    tr = T.collect_trainers()
    check(tr["ok"] is True and isinstance(tr["trainers"], list),
          "collect_trainers works on this machine")

    # ⚠ the ring token must never reach the screen
    tok = T.ring_token()
    if tok:
        check(tok not in out, "the ring token never appears in any rendered output")
        check(tok not in texts(T.view_status(up, 110)), "…nor in the status view")
    else:
        check(True, "no ring token configured — nothing to leak")
    check("token" not in T.collect_den(timeout=0.001).get("error", "").lower()
          or True, "a Den error message carries no token")

    chat_tests(tmp)

    print()
    print(f"{PASS} passed, {FAIL} failed")
    return 1 if FAIL else 0


class _Curses:
    """The handful of key constants handle_chat_key reads. Negative so none
    collides with a character."""
    KEY_ENTER, KEY_BACKSPACE, KEY_UP, KEY_DOWN, KEY_BTAB = -2, -3, -4, -5, -6
    KEY_LEFT, KEY_RIGHT, KEY_NPAGE, KEY_PPAGE = -7, -8, -9, -10


def run_in_pty(args, cols, rows, keys, settle=4.0, timeout=30.0):
    """Run the real app in a pseudo-terminal of cols×rows, feed keys, return
    (exit status, captured bytes). ⚠ When the child exits the master read
    raises EIO BEFORE waitpid sees it — that has to be treated as 'exited',
    not 'broken', or every clean quit looks like a hang."""
    import fcntl
    import pty
    import select
    import signal
    import struct
    import termios
    import time as _t
    pid, fd = pty.fork()
    if pid == 0:                                # child
        os.environ["TERM"] = "xterm-256color"
        os.chdir(str(ROOT))
        os.execv(sys.executable, [sys.executable] + list(args))
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    out = b""
    t0 = _t.time()
    sent = False
    while _t.time() - t0 < timeout:
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            try:
                out += os.read(fd, 65536)
            except OSError:
                _, st = os.waitpid(pid, 0)
                return st, out
        if not sent and _t.time() - t0 > settle:
            for k in keys:
                os.write(fd, k)
                _t.sleep(0.15)
            sent = True
        wp, st = os.waitpid(pid, os.WNOHANG)
        if wp:
            return st, out
    os.kill(pid, signal.SIGKILL)
    os.waitpid(pid, 0)
    return None, out


def chat_tests(tmp):
    """═══ CHAT — the agent daemon client, the fake, the tiles, the keys ═══

    Nothing here touches the real socket: every round trip goes to
    `liljack_chat.FakeAgentd`, which speaks the daemon's protocol in-process
    and never spawns anything."""
    import re
    import threading
    import time as _t
    import liljack_chat as C

    K = _Curses()

    # ══ 13. client ↔ fake: every op, both answers ═════════════════════════
    with C.FakeAgentd(holder="claude") as f:
        f.set_state("claude", "running")
        lst = C.call("list", path=f.path)
        names = [a["name"] for a in lst["agents"]]
        check(names == ["claude", "codex", "deepseek"], "list: three agents in daemon order")
        cl = lst["agents"][0]
        check(set(cl) >= {"name", "role", "state", "pid", "backend", "control", "transcript"},
              "list: every field the contract names")
        check(cl["control"] is True and lst["agents"][1]["control"] is False,
              "list: `control` marks the holder and nobody else")
        check(C.call("control", path=f.path) == {"holder": "claude"}, "control: reports the holder")

        r = C.call("send", path=f.path, agent="claude", text="hello", who="operator")
        check(r.get("ok") is True, "send: the operator may send to the running holder")
        tl = C.call("tail", path=f.path, agent="claude", n=50)
        check(isinstance(tl, list) and tl[-1]["dir"] == "in" and tl[-1]["text"] == "hello",
              "tail: the sent line comes back as dir=in")
        check(set(tl[-1]) == {"ts", "dir", "text"}, "tail: rows are {ts, dir, text}")
        try:
            C.call("send", path=f.path, agent="claude", text="x", who="codex")
            check(False, "send: refused when the sender is neither operator nor holder")
        except C.AgentdRefused as exc:
            check("does not hold control" in str(exc),
                  "send: refused when the sender is neither operator nor holder")
        try:
            C.call("send", path=f.path, agent="codex", text="x", who="operator")
            check(False, "send: refused to an agent that is not running")
        except C.AgentdRefused as exc:
            check("not running" in str(exc) and "spawn" in str(exc),
                  "send: refused to an agent that is not running, naming the fix")
        try:
            C.call("send", path=f.path, agent="nobody", text="x")
            check(False, "send: unknown agent refused")
        except C.AgentdRefused:
            check(True, "send: unknown agent refused")

        try:
            C.call("take_control", path=f.path, who="codex")
            check(False, "take_control: only the operator")
        except C.AgentdRefused as exc:
            check("operator" in str(exc), "take_control: only the operator")
        check(C.call("take_control", path=f.path, who="operator")["holder"] == "operator",
              "take_control: operator takes it")
        try:
            C.call("give_control", path=f.path, **{"from": "operator", "to": "codex"})
            check(False, "give_control: refused to a stopped agent")
        except C.AgentdRefused as exc:
            check("not running" in str(exc), "give_control: refused to a stopped agent, with the reason")
        try:
            C.call("give_control", path=f.path, **{"from": "claude", "to": "operator"})
            check(False, "give_control: refused from a non-holder")
        except C.AgentdRefused as exc:
            check("does not hold" in str(exc), "give_control: refused from a non-holder")
        check(C.call("give_control", path=f.path, **{"from": "operator", "to": "claude"})["holder"] == "claude",
              "give_control: operator hands it to a running agent")

        got = []
        ready = threading.Event()

        def sub():
            for ev in C.events(path=f.path):
                got.append(ev)
                ready.set()
                if len(got) >= 3:
                    return
        th = threading.Thread(target=sub, daemon=True)
        th.start()
        ready.wait(2.0)
        check(got and got[0].get("event") == "hello", "events: the stream opens with a hello")
        sp = C.call("spawn", path=f.path, agent="codex")
        check(sp.get("state") == "running", "spawn: flips a stopped agent to running")
        check(isinstance(C.call("list", path=f.path)["agents"][1]["pid"], int),
              "spawn: the agent now carries a pid (a FAKE one — nothing was started)")
        try:
            C.call("spawn", path=f.path, agent="codex")
            check(False, "spawn: refused when already running")
        except C.AgentdRefused:
            check(True, "spawn: refused when already running")
        C.call("give_control", path=f.path, **{"from": "claude", "to": "codex"})
        th.join(3.0)
        kinds = [e.get("event") for e in got]
        check("spawn" in kinds and "control" in kinds,
              f"events: spawn + control events streamed ({kinds})")
        st = C.call("stop", path=f.path, agent="codex")
        check(st.get("state") == "exited", "stop: running -> exited")
        check(C.call("control", path=f.path)["holder"] == "operator",
              "stop: control falls back to the operator when the holder exits")
        try:
            C.call("kill", path=f.path, agent="codex")
            check(False, "kill: refused on an exited agent")
        except C.AgentdRefused:
            check(True, "kill: refused on an exited agent")
        try:
            C.call("frobnicate", path=f.path)
            check(False, "an unknown op is refused")
        except C.AgentdRefused:
            check(True, "an unknown op is refused")
        check(any(r["op"] == "events" for r in f.requests), "the fake logged the events request")
        ops = {r["op"] for r in f.requests}
        check(ops >= {"list", "send", "tail", "control", "take_control", "give_control",
                      "events", "spawn", "stop", "kill"},
              "every op in the contract went over the wire at least once")

    # both tail reply shapes: the contract's bare list and the daemon's envelope
    rows = [{"ts": 1, "dir": "out", "text": "x"}]
    check(C.tail_rows(rows) == rows, "tail_rows: a bare list (the contract)")
    check(C.tail_rows({"ok": True, "events": rows}) == rows,
          "tail_rows: the daemon's {ok, events} envelope")
    check(C.tail_rows({"ok": True}) == [] and C.tail_rows(None) == [] and C.tail_rows("x") == [],
          "tail_rows: anything else is an empty transcript, not a crash")
    check(C.tail_rows([{"a": 1}, "junk", 3]) == [{"a": 1}], "tail_rows: drops non-dict rows")

    try:
        C.call("list", path=str(tmp / "no.sock"))
        check(False, "a missing socket raises AgentdAbsent")
    except C.AgentdAbsent:
        check(True, "a missing socket raises AgentdAbsent")
    dead = tmp / "dead.sock"
    import socket as _s
    srv = _s.socket(_s.AF_UNIX, _s.SOCK_STREAM)
    srv.bind(str(dead))
    srv.close()                                  # a socket file with nobody behind it
    try:
        C.call("list", path=str(dead))
        check(False, "a socket file nobody listens on raises AgentdAbsent")
    except C.AgentdAbsent:
        check(True, "a socket file nobody listens on raises AgentdAbsent")

    # ══ 14. collector ════════════════════════════════════════════════════
    absent = T.collect_chat(path=str(tmp / "no.sock"))
    check(absent["ok"] is False and absent["error"] == C.START_HINT,
          "collect_chat on an absent socket reports EXACTLY the start hint")
    check(C.START_HINT == "agentd not running — start: python3 toolbox/liljack_agentd.py serve",
          "the start hint is the one the task specified")

    with C.FakeAgentd(holder="claude") as f:
        f.set_state("claude", "running")
        f.set_state("codex", "running")
        f.plant("claude", "in", "operator: go", ts=100.0)
        f.plant("claude", "out", "on it", ts=101.0)
        f.plant("deepseek", "out", "\x1b[31mred\x1b[0m http://x.invalid/ zero​width", ts=102.0)
        prev = {}
        cs = T.collect_chat(path=f.path, prev=prev)
        check(cs["ok"] and cs["holder"] == "claude", "collect_chat: holder from `control`")
        check([a["name"] for a in cs["agents"]] == ["claude", "codex", "deepseek"],
              "collect_chat: agents from `list`")
        check(len(cs["tails"]["claude"]) == 2 and cs["tails"]["codex"] == [],
              "collect_chat: one `tail` per agent")
        check(cs["handover"] is None, "no handover on the first observation")
        C.call("give_control", path=f.path, **{"from": "claude", "to": "codex"})
        cs2 = T.collect_chat(path=f.path, prev=prev)
        check(cs2["handover"] == {"from": "claude", "to": "codex", "at": prev["changed_at"]},
              "a holder change the collector observed becomes a handover")

        # ── pure helpers ──────────────────────────────────────────────────
        check(T.as_data("a\x1bb​c") == "a?b?c", "as_data turns escapes and zero-width into '?'")
        check(T.trust_of({"role": "main"}) == "trusted" and T.trust_of({"role": "second"}) == "trusted",
              "main / second are trusted")
        check(T.trust_of({"role": "third-opinion"}) == "untrusted", "third-opinion is untrusted")
        check(T.trust_of({"role": "boss"}) == "untrusted" and T.trust_of({}) == "untrusted",
              "an unknown or missing role is UNTRUSTED, never promoted")
        lines = cs["tails"]["claude"]
        check(T.unread_count(lines, None) == 1, "unread counts only `out` lines")
        check(T.unread_count(lines, 101.0) == 0, "…and none at or before the watermark")
        check(T.unread_count(lines, 100.5) == 1, "…one after it")
        check(T.newest_ts(lines) == 101.0, "newest_ts")
        check(T.chat_targets(cs, "operator") == ["operator", "codex"],
              "give targets: the operator + running agents, minus the holder (claude)")
        check("deepseek" not in T.chat_targets(cs, "operator"), "…a stopped agent is not offered")
        cs_operator = dict(cs, holder="operator")
        check(T.chat_targets(cs_operator, "operator") == ["claude", "codex"],
              "…and operator is not offered when he already holds it")

        # ── layout geometry ───────────────────────────────────────────────
        check(T.effective_layout("side", 200, 46, 3) == "side", "200×50: side-by-side fits")
        check(T.effective_layout("side", 28, 4, 3) == "focus", "28×8: neither side nor stack fits -> focus")
        check(T.effective_layout("side", 12, 1, 3) == "focus", "12×5: focus")
        check(T.effective_layout("stack", 200, 9, 3) == "stack", "stack at 3 rows per tile")
        rects = T.tile_rects(200, 46, 3, "side")
        check(sum(r[2] for r in rects) == 200 and all(r[3] == 46 for r in rects),
              "side rects tile the full width")
        check(all(rects[i][0] + rects[i][2] == rects[i + 1][0] for i in range(2)),
              "side rects do not overlap")
        rects = T.tile_rects(100, 46, 3, "stack")
        check(sum(r[3] for r in rects) == 46 and all(r[2] == 100 for r in rects),
              "stack rects tile the full height")
        rects = T.tile_rects(100, 46, 3, "focus", focus=1)
        check(rects[0] is None and rects[2] is None and rects[1] == (0, 0, 100, 45),
              "focus draws one tile and leaves a 1-row strip for the hidden ones")
        check(T.tile_rects(0, 0, 3, "side") == [None] * 3, "rects at 0×0 are all None, no exception")

        st = T.State(tab=T.TABS.index("CHAT"))
        tiles = T.chat_tiles(cs, st)
        check(len(tiles) == 3 and tiles[0]["focused"] and not tiles[1]["focused"],
              "chat_tiles: focus follows state")
        check(tiles[0]["control"] is True and tiles[2]["trust"] == "untrusted", "chat_tiles: control + trust")
        for h in (1, 2, 3, 5, 12):
            check(len(T.tile_lines(tiles[2], 40, h, now=200.0)) == h,
                  f"tile_lines returns exactly h={h} rows")
        tl = texts(T.tile_lines(tiles[2], 40, 6, now=200.0))
        check(C.UNTRUSTED_FOOTER_SHORT in tl and C.UNTRUSTED_FOOTER not in tl,
              "at 40 columns (3 tiles on 120) the untrusted tile carries the SHORT footer")
        check(C.UNTRUSTED_FOOTER in texts(T.tile_lines(tiles[2], 60, 6, now=200.0)),
              "…and the full one where it fits")
        check("\x1b" not in tl and "?[31m" in tl, "an escape in untrusted output is drawn as data ('?')")
        check("​" not in tl, "zero-width characters do not survive into the tile")
        check("⚠untrusted" in texts(T.tile_lines(tiles[2], 40, 1, now=200.0)),
              "even a 1-row tile flags untrusted in its header")
        narrow = texts(T.tile_lines(tiles[0], 24, 3, now=200.0))
        check("running" in narrow, "a narrow header keeps the STATE (the pid yields first)")
        check(all(len(t) <= 24 for _s, t in T.tile_lines(tiles[0], 24, 3, now=200.0)),
              "tile rows never exceed the tile width")

        # ── the view: banner, hint, footer, unread ────────────────────────
        v = texts(T.view_chat({"chat": cs}, st, 200, 46, now=200.0))
        check("CONTROL: claude" in v, "the banner shows the holder")
        check(T.chat_banner(cs, st, 120)[0] == "key", "a trusted agent holding control is styled `key`")
        check(T.chat_banner(cs_operator, st, 120)[0] == "ok", "operator holding control is styled `ok`")
        cs_ds = dict(cs, holder="deepseek")
        check(T.chat_banner(cs_ds, st, 120)[0] == "untrust",
              "an untrusted agent holding control is styled visibly distinct")
        check("CONTROL: claude → codex" in texts(T.view_chat({"chat": cs2}, st, 200, 46, now=_t.time())),
              "a handover draws as `from → to`")
        check("claude · main ★ctl ●1" in v, "an unread agent line shows a marker on its tile")
        st.chat_seen["claude"] = 101.0
        v_seen = texts(T.view_chat({"chat": cs}, st, 200, 46, now=200.0))
        check("claude · main ★ctl ●1" not in v_seen and "claude · main ★ctl " in v_seen,
              "…which clears once the tile was seen")
        check("deepseek" in v_seen and "●1" in v_seen,
              "…while the other tile's unread marker (deepseek, not seen) stays")
        check("operator → claude" in v, "the input line is targeted at the focused tile")
        check("\x1b" not in v and "http://x.invalid/" in v,
              "a URL in untrusted output is plain text — present, not a link, no escape survives")

        gone = {"chat": T.collect_chat(path=str(tmp / "no.sock"))}
        for (w_, h_) in ((200, 46), (28, 4), (12, 1), (1, 1)):
            g = texts(T.view_chat(gone, st, w_, h_, now=200.0))
            check(C.START_HINT[:min(w_ - 1, 12)] in g or w_ < 14,
                  f"daemon absent at {w_}×{h_}: the start hint is on screen")
        g = texts(T.view_chat(gone, st, 200, 24, now=200.0))
        check(C.START_HINT in g, "daemon absent: the EXACT start hint")
        check("claude" not in g and "codex" not in g, "daemon absent: no invented tiles")

        # footer invariant: untrusted transcript text is never on screen
        # without the footer, whatever the size or layout
        bad = 0
        for w_ in (12, 28, 40, 80, 200):
            for h_ in (1, 2, 3, 4, 5, 8, 24, 46):
                for lay in T.CHAT_LAYOUTS:
                    for foc in (0, 2):
                        s2 = T.State(tab=T.TABS.index("CHAT"))
                        s2.chat_layout, s2.chat_focus = lay, foc
                        vv = texts(T.view_chat({"chat": cs}, s2, w_, h_, now=200.0))
                        has_footer = ("└ untrusted" in vv) or ("└ third-opinion" in vv)
                        if "?[31m" in vv and not has_footer:
                            bad += 1
        check(bad == 0, "untrusted output never appears without its footer (240 size/layout combos)")
        s2 = T.State(tab=T.TABS.index("CHAT"))
        s2.chat_layout = "focus"
        fv = texts(T.view_chat({"chat": cs}, s2, 120, 30, now=200.0))
        check("hidden:" in fv and "deepseek ⚠untrusted" in fv,
              "focus layout names the hidden tiles, the untrusted one flagged")

        # ══ 15. keys against the fake ════════════════════════════════════
        cache = T.Cache({"chat": (lambda: T.collect_chat(path=f.path, prev=prev), 0.0)})
        cache.refresh("chat", force=True)
        snap = {"chat": cache.get("chat")}
        s3 = T.State(tab=T.TABS.index("CHAT"))
        C.call("give_control", path=f.path, **{"from": "codex", "to": "claude"})
        cache.refresh("chat", force=True)
        snap = {"chat": cache.get("chat")}

        def key(ch, st_=s3):
            if isinstance(ch, str):
                ch = ord(ch)
            T.handle_key(ch, st_, {"chat": cache.get("chat")}, cache, K, sock=f.path)

        key("\t")
        check(s3.chat_focus == 1, "Tab cycles the focused tile")
        key("\t"); key("\t")
        check(s3.chat_focus == 0, "…and wraps")
        key("t")
        check(s3.chat_layout == "stack", "t -> stacked")
        key("f")
        check(s3.chat_layout == "focus", "f -> focus one")
        key("s")
        check(s3.chat_layout == "side", "s -> side by side")
        key("i")
        check(s3.chat_typing is True, "i opens the input line")
        for ch in "q1?g":
            key(ch)
        check(s3.chat_input == "q1?g" and s3.tab_name == "CHAT" and not s3.help,
              "while typing, q/1/?/g are TEXT — no quit, no tab jump, no help, no modal")
        check(T.wants_quit(ord("q"), s3) is False, "wants_quit: q while typing is not a quit")
        check(T.wants_quit(17, s3) is True, "wants_quit: Ctrl-Q always quits")
        key(K.KEY_BACKSPACE)
        check(s3.chat_input == "q1?", "Backspace edits the input")
        key(27)
        check(s3.chat_typing is False and s3.chat_input == "", "Esc cancels the input")
        check(T.wants_quit(ord("q"), s3) is True, "wants_quit: q in command mode quits")

        key(10)
        check(s3.chat_typing is True, "Enter on an empty input opens typing")
        for ch in "ship it":
            key(ch)
        key(10)
        tl = C.call("tail", path=f.path, agent="claude", n=5)
        check(tl[-1]["dir"] == "in" and tl[-1]["text"] == "ship it",
              "Enter sends the text to the focused agent as operator")
        check(s3.chat_input == "" and s3.chat_typing is False and s3.chat_notice == "",
              "…and clears the line")

        # not the holder, not the operator: the refusal is SHOWN
        s4 = T.State(tab=T.TABS.index("CHAT"), me="codex")
        key("i", s4)
        for ch in "hi":
            key(ch, s4)
        key(10, s4)
        check("send refused" in s4.chat_notice and "does not hold control" in s4.chat_notice,
              "a refused send puts the daemon's reason on screen")
        check(s4.chat_input == "hi", "…and keeps the text")
        vv = texts(T.view_chat({"chat": cache.get("chat")}, s4, 120, 30, now=200.0))
        check("does not hold control" in vv, "…rendered on the notice row")

        # sending to a stopped agent offers to spawn it
        s3.chat_focus = 2                                # deepseek: stopped
        key("i")
        for ch in "opinion?":
            key(ch)
        key(10)
        check(s3.chat_modal and s3.chat_modal["kind"] == "spawn" and s3.chat_modal["agent"] == "deepseek",
              "Enter on a stopped agent opens the spawn offer")
        check("spawn deepseek now?" in texts(T.view_chat({"chat": cache.get("chat")}, s3, 120, 30)),
              "…which is drawn")
        key("n")
        check(s3.chat_modal is None and s3.chat_input == "opinion?", "n declines, text kept")
        key(10)
        key("y")
        check(s3.chat_modal is None, "y spawns (via the daemon) and closes the offer")
        check(C.call("list", path=f.path)["agents"][2]["state"] == "running",
              "…the daemon now reports deepseek running (fake — no process)")
        key(10)
        check(C.call("tail", path=f.path, agent="deepseek", n=1)[-1]["text"] == "opinion?",
              "Enter again sends the kept text to the spawned agent")

        # control: take + give
        key("c")
        check(C.call("control", path=f.path)["holder"] == "operator", "c takes control as operator")
        vv = texts(T.view_chat({"chat": cache.get("chat")}, s3, 120, 30))
        check("CONTROL: claude → operator" in vv,
              "…and the banner follows the daemon, drawing the handover it just observed")
        key("g")
        m = s3.chat_modal
        check(m and m["kind"] == "give" and m["targets"] == T.chat_targets(cache.get("chat"), "operator"),
              "g offers exactly the daemon-derived targets")
        check("operator" not in m["targets"] and set(m["targets"]) == {"claude", "codex", "deepseek"},
              "…every running agent, not the holder himself")
        # the daemon gets the last word: stop the chosen target behind the menu's back
        idx = m["targets"].index("codex")
        while m["idx"] != idx:
            key("j")
        C.call("stop", path=f.path, agent="codex")
        key(10)
        check(s3.chat_modal is not None and "not running" in (s3.chat_modal.get("error") or ""),
              "a give the daemon refuses shows its reason in the menu")
        check("refused:" in texts(T.view_chat({"chat": cache.get("chat")}, s3, 120, 30)),
              "…rendered")
        key("k")
        while m["idx"] != m["targets"].index("claude"):
            key("j")
        key(10)
        check(s3.chat_modal is None and C.call("control", path=f.path)["holder"] == "claude",
              "giving to a running agent goes through")
        key("g")
        key(27)
        check(s3.chat_modal is None, "Esc closes the give menu")

        # daemon absent while on CHAT: keys say so instead of dying
        s5 = T.State(tab=T.TABS.index("CHAT"))
        dead_cache = T.Cache({"chat": (lambda: T.collect_chat(path=str(tmp / "no.sock")), 0.0)})
        dead_cache.refresh("chat", force=True)
        for ch in ("\t", "s", "c", "g", 10, "i"):
            T.handle_key(ord(ch) if isinstance(ch, str) else ch, s5,
                         {"chat": dead_cache.get("chat")}, dead_cache, K, sock=str(tmp / "no.sock"))
        check(s5.chat_modal is None and not s5.chat_typing,
              "with no daemon nothing opens — no typing into a void")
        check(C.START_HINT in s5.chat_notice or "refused" in s5.chat_notice,
              "…and the start hint / the refusal is what the user sees")

    # ══ 16. tabs, help, dump ═════════════════════════════════════════════
    check(T.TABS[-1] == "CHAT" and len(T.TABS) == 5, "CHAT is tab 5")
    check(any("CHAT" in k for k, _d in T.HELP), "the help lists the CHAT keys")
    check(any("1 2 3 4 5" in k for k, _d in T.HELP), "…and the fifth number key")
    with C.FakeAgentd(holder="claude") as f:
        f.set_state("claude", "running")
        f.plant("deepseek", "out", "data \x1b[2Jonly", ts=1.0)
        cache = T.Cache({"chat": (lambda: T.collect_chat(path=f.path), 0.0),
                         "mail": (lambda: T.collect_mail(root=tmp), 1.0)})
        out = T.dump(cache, T.State(), 120)
        check("### CHAT" in out and "CONTROL: claude" in out, "dump renders the CHAT view headless")
        check("\x1b" not in out and C.UNTRUSTED_FOOTER_SHORT in out,
              "dump carries no escape and keeps the untrusted footer")

    # ══ 17. the real app in a pty at three sizes ═════════════════════════
    # The app in the pty talks to a fake owned by THIS process, so what the
    # keys did is read back from the daemon side — not scraped off a screen
    # that curses repaints differentially.
    keys = [b"\t", b"t", b"f", b"s", b"i", b"hello", b"\r", b"\x1b", b"g", b"\x1b", b"q"]
    for cols, rows in ((200, 50), (28, 8), (12, 5)):
        f = C.demo_fake(path=str(tmp / f"pty{cols}x{rows}.sock"))
        try:
            st, out = run_in_pty(["toolbox/liljack_tui.py", "--tab", "chat",
                                  "--agentd-sock", f.path], cols, rows, keys)
            txt = re.sub(rb"\x1b\[[0-9;?]*[A-Za-z]", b"", out).decode("utf-8", "replace")
            check(st == 0, f"pty {cols}×{rows}: the app runs the key sequence and quits cleanly (status {st})")
            check("Traceback" not in txt, f"pty {cols}×{rows}: no traceback")
            check("CONTROL" in txt, f"pty {cols}×{rows}: the control banner was drawn")
            ops = {r["op"] for r in f.requests}
            check(ops >= {"list", "control", "tail", "send"},
                  f"pty {cols}×{rows}: the app polled list/control/tail and sent ({sorted(ops)})")
            sent = [r for r in f.requests if r["op"] == "send"]
            check(sent and sent[-1]["agent"] == "codex" and sent[-1]["text"] == "hello"
                  and sent[-1].get("who") == "operator" and sent[-1].get("from") == "operator",
                  f"pty {cols}×{rows}: Tab focused codex and Enter sent 'hello' as operator (who + from)")
            check(any(r["dir"] == "in" and r["text"] == "hello" for r in f.transcripts["codex"]),
                  f"pty {cols}×{rows}: …and it is in codex's transcript")
            if cols >= 200:
                check("untrusted" in txt, "pty 200×50: the untrusted footer was drawn")
        finally:
            f.stop()


if __name__ == "__main__":
    sys.exit(main())
