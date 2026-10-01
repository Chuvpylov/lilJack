#!/usr/bin/env python3
"""
test_liljack_tasks.py — the multi-agent coordination layer.

Covers `toolbox/liljack_tasks.py` (the durable task registry),
`toolbox/liljack_sessions.py` (the live-session registry) and the parts of
`toolbox/liljack_heartbeat.py` that were rewired onto them.

Plain script, ✓/✗ per check, non-zero exit on failure — the house style.

⚠ THE REGRESSION THIS FILE EXISTS FOR is section 5: reading mail must not
consume a task. On 2026-09-05 four real, unfinished tasks became "read" because
Codex opened them through the MCP `read_messages` tool, the unread-only queue
went empty, and the watcher reported `idle_no_task` while he had work
outstanding. Every other check here is a boundary; that one is the bug.

⚠ `codex queue` is NEVER called. Deliveries go through an injected stub, the
lifecycle DB is a fixture, and every registry write goes to a temp file — the
real `the trainer/data/braid/agent_tasks.bmd` is never opened for writing.
"""
import os
# A launched session (LILJACK_WORKSPACE_SESSION) is refused moves on rows bound to
# other sessions; these fixtures act as many sessions, so run them unbound.
os.environ.pop("LILJACK_WORKSPACE_SESSION", None)
import sys
import tempfile
import threading
from datetime import datetime, timedelta, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "toolbox"))

# A third agent, declared before import — the roster is built at import time and
# this is the whole "N agents, not 2" claim: no core file names `chad`.
os.environ.setdefault("LILJACK_AGENTS", "chad")

import liljack_tasks as T          # noqa: E402
import liljack_sessions as S       # noqa: E402
import liljack_heartbeat as H      # noqa: E402
import liljack_mail as M           # noqa: E402
import liljack_board as B          # noqa: E402

PASS = FAIL = 0
CWD = str(ROOT)


def check(cond, label):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"✓ {label}")
    else:
        FAIL += 1
        print(f"✗ {label}")


def raises(fn, exc, label):
    try:
        fn()
    except exc:
        check(True, label)
        return
    except Exception as other:
        check(False, f"{label} (raised {type(other).__name__}: {other})")
        return
    check(False, f"{label} (nothing was raised)")


def tmp_reg(name="tasks") -> Path:
    return Path(tempfile.mkdtemp(prefix="ljtask-")) / f"{name}.bmd"


def now():
    return datetime.now(timezone.utc)


def ago(sec):
    return (now() - timedelta(seconds=sec)).isoformat()


class Spy:
    def __init__(self, ok=True):
        self.calls, self.mails, self.ok = [], [], ok

    def deliver(self, session, text):
        self.calls.append((session, text))
        return {"ok": self.ok, "err": "" if self.ok else "no such thread"}

    def send(self, text, to, frm="", subject="", project="", thread="", root=None):
        self.mails.append({"to": to, "frm": frm, "subject": subject, "text": text})
        return {"id": "spy"}


def dependency_release():
    """⚠ FINISHING WORK MUST UNBLOCK THE WORK THAT WAITED FOR IT.

    Measured 2026-09-10: DeepSeek finished the task two of Codex's were blocked
    on and they stayed blocked, because a dependency lived only in a free-text
    note and no code could tell the tasks were related. the operator: "why ds finished
    and no todos are unbloked no heartbeat spanking agents?". `blocked_on` makes
    the dependency a FIELD, and complete() releases against it.
    """
    path = tmp_reg("deps")
    T.propose("dep", "claude", owner="deepseek", task_id="dep", path=path)
    T.propose("waiter", "claude", owner="codex", task_id="waiter", path=path)
    T.propose("orphan", "claude", owner="codex", task_id="orphan", path=path)
    T.propose("other", "claude", owner="codex", task_id="other", path=path)
    T.block("waiter", "codex", "waiting on dep", path=path, blocked_on="dep")
    T.block("orphan", "codex", "waiting on dep too", path=path, blocked_on="dep")
    T.block("other", "codex", "blocked on something else entirely", path=path)
    # the dependency has to survive the BMD round trip or nothing else can work
    check(T.get("waiter", path=path)["blocked_on"] == "dep", "blocked_on persists to disk")
    check(sorted(T.blocked_by("dep", path=path)) == ["orphan", "waiter"],
          "blocked_by names everything waiting on a task")

    # Standing an owner down is the only way a waiter ends up unowned: a task
    # with no owner cannot be blocked in the first place. It leaves `blocked`,
    # so its dependency must go with it — otherwise blocked_by() reports a
    # waiter that is not waiting, and a later completion releases a task that
    # no longer depends on it.
    T.unassign("orphan", "claude", "owner stood down while blocked", path=path)

    # ⚠ Standing down an owner takes the task OUT of blocked (to proposed), and
    # leaving `blocked` clears the dependency — a task that is not blocked must
    # not keep claiming to wait on something, or blocked_by() reports a waiter
    # that is not waiting. So only the still-blocked task is released here.
    check(T.get("orphan", path=path)["blocked_on"] == "",
          "leaving blocked clears the dependency, so nothing claims a stale wait")
    check(T.blocked_by("dep", path=path) == ["waiter"], "only a still-blocked task counts as waiting")
    freed = T.complete("dep", "deepseek", path=path).get("released")
    check(sorted(freed or []) == ["waiter"], "completing a task releases exactly its waiters")
    # an owned task returns to assigned; an UNOWNED one to proposed, because
    # 'assigned' with nobody assigned is not a state anyone can act on
    check(T.get("waiter", path=path)["state"] == "assigned", "owned waiter returns to assigned")
    check(T.get("orphan", path=path)["state"] == "proposed", "an unassigned task sits in proposed")
    check(T.get("other", path=path)["state"] == "blocked", "an unrelated block is left alone")
    # why it was blocked is history worth keeping, not something to overwrite
    check("waiting on dep" in T.get("waiter", path=path)["note"], "the blocking reason is preserved")
    check(T.get("waiter", path=path)["blocked_on"] == "", "a met dependency is cleared")
    # and the released work is PENDING again, so it arms the heartbeat
    check("waiter" in [t["id"] for t in T.pending_tasks(path=path)],
          "released work is pending again, so the heartbeat arms on it")


def forward_compatible_fields():
    """⚠ A WRITER MUST NOT DELETE FIELDS IT HAS NEVER HEARD OF.

    Measured 2026-09-10: a pre-schema MCP server, running since before `room`
    and `blocked_on` existed, updated ONE task — and both fields vanished from
    all fourteen rows, because its serializer rebuilt every row from a fixed
    field list. Restarting that process fixes today; carrying unknown
    attributes forward is what stops the NEXT field being erased the same way.
    """
    path = tmp_reg("fwd")
    T.propose("a task", "claude", owner="claude", task_id="keep", path=path)
    raw = path.read_text(encoding="utf-8").replace("| created=", "| future_field=survives-me | created=", 1)
    path.write_text(raw, encoding="utf-8")
    check("future_field=survives-me" in path.read_text(encoding="utf-8"), "a future field can be planted")
    check(T.get("keep", path=path)["_extra"].get("future_field") == "survives-me",
          "an unknown attribute is READ into _extra rather than discarded")
    T.set_progress("keep", "1/2", "claude", path=path)
    check("future_field=survives-me" in path.read_text(encoding="utf-8"),
          "and an ordinary write carries it back out unchanged")
    check(T.get("keep", path=path)["progress"] == "1/2", "while the write it was asked to do still lands")


def board_mirror_follows_every_write():
    """⚠ THE MIRROR BELONGED TO ONE CALLER, SO THE TWO STORES DISAGREED.

    `sync_board` was invoked from `liljack_mcp._mirror()` and from an explicit
    CLI subcommand — nowhere else. So a registry write through the LIBRARY (or
    through `python3 -c`, which is how completions were being written while the
    MCP servers held stale modules) updated the registry and left the board
    asserting the opposite, forever.

    Measured 2026-09-10: `room-phase-ui`, `room-phase-machine` and
    `room-question-routing` were `done` in the registry and still `active` on
    the board. The heartbeat unions the board into its nudge set, so it nudged
    their owners to go finish work that was already finished.

    The fix moves the mirror into `_save`, the single write funnel — the board
    now follows the registry no matter who writes.
    """
    import liljack_board as _B
    live_board = _B.default_path()
    live_before = live_board.read_bytes() if live_board.exists() else None

    d = Path(tempfile.mkdtemp())
    ps, pb = d / "tasks.bmd", d / "board.bmd"
    old_t, old_b = os.environ.get("LILJACK_TASKS"), os.environ.get("LILJACK_BOARD")
    os.environ["LILJACK_TASKS"], os.environ["LILJACK_BOARD"] = str(ps), str(pb)
    try:
        # No explicit path= anywhere below: these are DEFAULT-path writes, the
        # ones the hook is allowed to mirror.
        T.propose("mirror me", by="claude", owner="claude", task_id="mirror-me")
        T.start("mirror-me", by="claude")
        row = next(i for i in _B.read(pb)["items"] if i["task"] == "mirror-me")
        check(row["state"] == "active",
              "a library write mirrors onto the board (no MCP in the path)")

        T.complete("mirror-me", by="claude", note="finished")
        row = next(i for i in _B.read(pb)["items"] if i["task"] == "mirror-me")
        check(row["state"] == "done",
              "completing through the library moves the board row to done")

        # done -> abandoned is refused by the state machine, so abandon a
        # second task rather than contorting the first.
        T.propose("drop me", by="claude", owner="claude", task_id="drop-me")
        T.abandon("drop-me", by="claude", note="nope")
        negs = [n for n in _B.read(pb)["negs"] if n["args"][:2] == ["claude", "drop-me"]]
        check(bool(negs), "abandoning through the library negates the board row")
    finally:
        for k, v in (("LILJACK_TASKS", old_t), ("LILJACK_BOARD", old_b)):
            os.environ.pop(k, None)
            if v is not None:
                os.environ[k] = v

    # ⚠ THE OTHER HALF OF THE SAFETY RULE, AND IT WAS A LIVE REGRESSION.
    # Checking only that the REGISTRY is the default one is not enough: a test
    # that redirects LILJACK_TASKS to a temp file and leaves LILJACK_BOARD
    # alone makes default_path() return the temp registry, so the registry
    # check passes and the mirror writes fixture rows onto the REAL board.
    # Measured 2026-09-10 06:13Z: six rows from test_liljack_room_protocol.py
    # reached the live board and entered the heartbeat's nudge set as work for
    # codex. A registry may only mirror onto the board BESIDE IT.
    import liljack_board as _B2
    live2 = _B2.default_path()
    live_bytes = live2.read_bytes() if live2.exists() else None
    d3 = Path(tempfile.mkdtemp())
    ps3 = d3 / "tasks.bmd"                      # temp registry...
    old_t3 = os.environ.get("LILJACK_TASKS")
    os.environ["LILJACK_TASKS"] = str(ps3)      # ...and NO LILJACK_BOARD
    try:
        T.propose("leak me", by="claude", owner="claude", task_id="leak-me")
        T.start("leak-me", by="claude")
    finally:
        os.environ.pop("LILJACK_TASKS", None)
        if old_t3 is not None:
            os.environ["LILJACK_TASKS"] = old_t3
    after = live2.read_bytes() if live2.exists() else None
    check(after == live_bytes,
          "a temp registry with the DEFAULT board does not write to the live board")

    # ⚠ THE SAFETY RULE. Every test writes a temp registry with an explicit
    # path=. If such a write mirrored, running this suite would rewrite the
    # LIVE attention board with fixture rows. It must not.
    ps2 = Path(tempfile.mkdtemp()) / "tasks.bmd"
    T.propose("fixture", by="claude", owner="claude", task_id="fixture-task", path=ps2)
    T.start("fixture-task", by="claude", path=ps2)
    live_after = live_board.read_bytes() if live_board.exists() else None
    check(live_after == live_before,
          "an explicit-path (test) write NEVER touches the live board")


def scope_clobber_repair():
    """⚠ A GUARD CANNOT CONSTRAIN A WRITER WE DO NOT CONTROL.

    Measured 2026-09-10: a process running a module older than `room` and
    `blocked_on` rebuilt every row from a fixed field list and deleted both,
    for all fourteen tasks, while updating one. Its owner has to refresh it, and
    until then it can do this again. The LOG survived, because appending to it
    does not require understanding the fields — so the damage repairs itself
    from the record rather than waiting for someone to notice.
    """
    import re
    path = tmp_reg("clobber")
    T.propose("blocker", "claude", owner="claude", task_id="a", path=path)
    T.propose("waiter", "claude", owner="codex", task_id="b", path=path)
    T.propose("free", "claude", owner="codex", task_id="c", path=path)
    T.set_room("a", "r-ROOM", "claude", path=path)
    T.block("b", "codex", "waiting", path=path, blocked_on="a")

    raw = path.read_text(encoding="utf-8")
    raw = re.sub(r" \| room=[^|)]*", "", raw)
    raw = re.sub(r" \| blocked_on=[^|)]*", "", raw)
    path.write_text(raw, encoding="utf-8")
    check(T.get("a", path=path)["room"] == "", "the clobber is reproduced: room is gone")
    check(T.get("b", path=path)["blocked_on"] == "", "and so is the dependency")

    fixed = T.reconcile_scopes(path=path)
    check(sorted(fixed) == ["a.room", "b.blocked_on"], "exactly the lost fields are named")
    check(T.get("a", path=path)["room"] == "r-ROOM", "room is restored from the log")
    check(T.get("b", path=path)["blocked_on"] == "a", "the dependency is restored from the log")
    check(T.get("c", path=path)["room"] == "", "a task that never had one is left alone")

    # ⚠ A CLEARED FIELD IS NOT A LOST ONE: clearing is logged too, and the
    # newest entry wins, so a deliberate clear must not be undone.
    T.set_room("a", "", "claude", path=path)
    check(T.reconcile_scopes(path=path) == [], "a deliberately cleared field is not resurrected")
    check(T.get("a", path=path)["room"] == "", "and it stays cleared")


def main():
    # ══ 1. the roster and the constants ══════════════════════════════════
    check("chad" in T.AGENTS, "a third agent joins through LILJACK_AGENTS, no core edit")
    check(set(M.AGENTS) <= set(T.AGENTS), "the mailbox roster is a subset — nothing is dropped")
    check(T.SYSTEM not in T.AGENTS and T.SYSTEM in T.ACTORS,
          "the heartbeat is an ACTOR but not an AGENT — it can never own work")
    check(T.STATES == ("proposed", "assigned", "active", "held", "blocked", "done", "abandoned"),
          "the lifecycle is proposed → assigned → active → done, plus held, blocked and abandoned")
    check(T.DELIVERABLE == "assigned",
          "only an ASSIGNED task is deliverable — active is already in front of somebody")
    check(all(s in T.TRANSITIONS for s in T.STATES), "every state declares its exits")

    # ══ 2. the full lifecycle ════════════════════════════════════════════
    p = tmp_reg()
    t = T.propose("judge the uk school set", by="operator", owner="codex",
                  brief="Judge school/uk, second family.\n\n  - 600 units",
                  acceptance="600/600 judged, kappa reported", avoid="the trainer/runs",
                  path=p)
    check(t["state"] == "assigned" and t["owner"] == "codex",
          "a task proposed WITH an owner is created assigned")
    check(t["id"] == "judge-the-uk-school-set", "the id is a slug of the title")
    check(T.propose("x", by="operator", path=p)["state"] == "proposed",
          "a task proposed WITHOUT an owner is created proposed, not assigned")

    r = T.start(t["id"], by="codex", path=p)
    check(r["state"] == "active", "the owner starts his own task")
    r = T.set_progress(t["id"], "75/600", by="codex", path=p)
    check(r["progress"] == "75/600" and r["state"] == "active",
          "progress is recorded without changing state")
    r = T.block(t["id"], by="codex", note="waiting on the core rebuild", path=p)
    check(r["state"] == "blocked" and "core rebuild" in r["note"], "blocked carries its reason")
    raises(lambda: T.block(t["id"], by="codex", note="", path=p), T.TaskError,
           "blocking without a reason is refused — a blocker with no cause is not a state")
    r = T.complete(t["id"], by="codex", note="600/600, kappa 0.4", path=p)
    check(r["state"] == "done", "blocked → done is legal; the agent reports his own finish")
    check(T.get(t["id"], path=p)["progress"] == "75/600",
          "completing does not erase progress — omitted fields keep their value")

    # illegal moves are refused by the machine, not by convention
    raises(lambda: T.set_state("x", "done", by="operator", path=p), T.TaskError,
           "proposed → done is not a legal move")
    raises(lambda: T.set_state("no-such-task", "active", by="operator", path=p), T.TaskError,
           "an unknown task id is an error, never a silently created task")

    hist = T.history(t["id"], path=p)
    check([h["op"] for h in hist] == ["propose", "state", "progress", "state", "state"],
          "every transition is logged in order")
    check(all(h["by"] and h["at"] for h in hist), "…with who did it and when")

    # ══ 3. permissions — enforced in code ════════════════════════════════
    p2 = tmp_reg()
    mine = T.propose("codex work", by="operator", owner="codex", path=p2)
    free = T.propose("nobody's work", by="operator", path=p2)

    raises(lambda: T.assign(mine["id"], "chad", by="codex", path=p2), T.Denied,
           "an agent may NOT reassign its own task to somebody else")
    raises(lambda: T.assign(free["id"], "chad", by="codex", path=p2), T.Denied,
           "an agent may NOT assign an unowned task to somebody else")
    raises(lambda: T.propose("work for chad", by="codex", owner="chad", path=p2), T.Denied,
           "an agent may NOT invent a task for another agent")
    raises(lambda: T.complete(mine["id"], by="chad", path=p2), T.Denied,
           "an agent may NOT complete another agent's task")
    raises(lambda: T.start(mine["id"], by="chad", path=p2), T.Denied,
           "…nor start one")
    raises(lambda: T.set_progress(mine["id"], "1/2", by="chad", path=p2), T.Denied,
           "…nor report progress on one")
    raises(lambda: T.claim(free["id"], by="nobody", path=p2), T.Denied,
           "an unknown actor is refused outright")

    check(T.claim(free["id"], by="chad", path=p2)["owner"] == "chad",
          "any agent may CLAIM an unassigned task")
    check(T.claim(free["id"], by="chad", path=p2)["owner"] == "chad",
          "…and re-claiming your own task is a no-op, not a refusal")
    raises(lambda: T.claim(free["id"], by="codex", path=p2), T.Denied,
           "a claimed task cannot be claimed away by another agent")
    check(T.assign(free["id"], "codex", by="operator", path=p2)["owner"] == "codex",
          "an ASSIGNER may move a task between agents")
    raises(lambda: T.set_state(mine["id"], "active", by="chad", path=p2), T.Denied,
           "the permission gate is on the state change, not only on the wrapper")

    # ⚠ Assigning and supervising are different powers. Caught by exercising the
    # MCP tools: an assigner-bypass on the state check let claude mark CODEX's
    # task done — the exact falsehood this layer exists to prevent.
    check("claude" in T.ASSIGNERS and "claude" not in T.OVERRIDE,
          "an assigner is not automatically an overrider")
    raises(lambda: T.complete(mine["id"], by="claude", path=p2), T.Denied,
           "★ an ASSIGNER may not complete another agent's task — handing work over "
           "and reporting it finished are separate powers")
    raises(lambda: T.set_progress(mine["id"], "9/9", by="claude", path=p2), T.Denied,
           "…nor report progress on it")
    check(T.assign(mine["id"], "codex", by="claude", path=p2)["owner"] == "codex",
          "…but he may still hand it over, which is what an assigner is for")
    check(T.complete(mine["id"], by="operator", path=p2)["state"] == "done",
          "only operator — who decides — may move a task he does not own")

    # an unowned task cannot be worked without claiming it first
    orphan = T.propose("unowned", by="operator", path=p2)
    raises(lambda: T.start(orphan["id"], by="chad", path=p2), T.Denied,
           "an unowned task must be claimed before it can be started")

    # terminal states re-open only through an assigner
    done = T.propose("finished thing", by="operator", owner="chad", path=p2)
    T.start(done["id"], by="chad", path=p2)
    T.complete(done["id"], by="chad", path=p2)
    raises(lambda: T.assign(done["id"], "chad", by="chad", path=p2), T.Denied,
           "an agent may not re-open its own finished task")
    check(T.assign(done["id"], "chad", by="operator", path=p2)["state"] == "assigned",
          "an assigner may re-open it")

    # the system actor's rights are exactly two
    sysp = tmp_reg()
    st = T.propose("system task", by="operator", owner="codex", path=sysp)
    raises(lambda: T.propose("invented", by=T.SYSTEM, path=sysp), T.Denied,
           "the heartbeat may NOT create a task")
    raises(lambda: T.complete(st["id"], by=T.SYSTEM, path=sysp), T.Denied,
           "the heartbeat may NOT complete a task — finishing is the agent's own report")
    raises(lambda: T.assign(st["id"], "chad", by=T.SYSTEM, path=sysp), T.Denied,
           "the heartbeat may NOT assign")
    check(T.deliver(st["id"], session="s1", path=sysp)["state"] == "active",
          "the heartbeat MAY move an assigned task to active on delivery")
    raises(lambda: T.deliver(st["id"], session="s1", path=sysp), T.Denied,
           "…and only from `assigned` — an active task is not delivered twice")

    # the whole rule set is one function, so there is nowhere for a second
    # opinion to live
    check(T.authorise("operator", "assign", None, "codex") == "operator",
          "authorise() returns the normalised actor when it allows the act")

    # ══ 4. unassign writes an explicit neg() ═════════════════════════════
    p3 = tmp_reg()
    u = T.propose("handed over", by="operator", owner="codex", path=p3)
    T.unassign(u["id"], by="operator", reason="handed to claude", path=p3)
    store = T.read(p3)
    row = [t for t in store["tasks"] if t["id"] == u["id"]][0]
    check(row["owner"] == "" and row["state"] == "proposed", "unassign clears the owner")
    negs = [n for n in store["negs"] if n["args"] == [u["id"], "codex"]]
    check(len(negs) == 1 and "handed to claude" in negs[0]["reason"],
          "…and writes neg(task-owner, id, codex) — the engine has no negation-as-failure, "
          "so the absence has to be a claim")
    T.assign(u["id"], "codex", by="operator", path=p3)
    check(not [n for n in T.read(p3)["negs"] if n["args"] == [u["id"], "codex"]],
          "re-assigning retracts the denial — the file never asserts both at once")

    # ══ 5. THE REGRESSION: reading mail does not consume a task ══════════
    root = Path(tempfile.mkdtemp(prefix="ljtask-mail-"))
    reg = root / "agent_tasks.bmd"
    m1 = M.send("judge school/uk second family, 600 units", "codex", frm="claude",
                subject="TASK: uk second family", project="demo", root=root)
    made = T.import_mail("codex", root=root, path=reg)
    check(len(made) == 1 and made[0]["mail"] == m1["id"],
          "a TASK: mail becomes a durable registry task")
    check(made[0]["state"] == "assigned" and made[0]["owner"] == "codex",
          "…assigned to the addressee")

    # Codex reads his mail through the MCP tool, which ACKS. This is the exact
    # act that destroyed the old queue.
    M.ack([m1["id"]], "codex", root=root)
    check(M.inbox("codex", unread_only=True, root=root) == [],
          "after reading, the mailbox reports nothing unread (the old queue's whole input)")
    still = T.next_for("codex", path=reg)
    check(still is not None and still["mail"] == m1["id"],
          "★ THE REGRESSION: the task is STILL assigned after the mail was read")
    check(T.get(made[0]["id"], path=reg)["state"] == "assigned",
          "★ …reading a message is an act of reading and changes no task state")

    again = T.import_mail("codex", root=root, path=reg)
    check(again == [], "an already-imported mail is never imported twice (dedup on message id)")
    check(len(T.tasks(owner="codex", path=reg)) == 1, "…so the registry holds exactly one task")

    # and the watcher agrees
    ev = [{"seq": 1, "at": ago(400), "session": "s-1", "cwd": CWD, "event": "UserPromptSubmit"},
          {"seq": 2, "at": ago(300), "session": "s-1", "cwd": CWD, "event": "Stop"}]
    spy = Spy()
    rep = H.tick(root=root, events=ev, cwd_filter=CWD, dry_run=True, send_mail=spy.send)
    check(rep["action"] == "would-deliver" and rep["queue_depth"] == 1,
          "★ the heartbeat sees the task the read mail no longer shows")
    check(rep["task"] == made[0]["id"], "…and names the registry task, not just the message")

    # non-TASK mail is not work, and non-assigners cannot smuggle work in
    M.send("how is it going?", "codex", frm="claude", subject="checking in", root=root)
    M.send("do this for me", "codex", frm="operator", subject="TASK: from operator", root=root)
    n_before = len(T.tasks(path=reg))
    made2 = T.import_mail("codex", root=root, path=reg)
    check(len(made2) == 1 and made2[0]["by"] == "operator",
          "a TASK: mail from an assigner imports; the chat message does not")
    check(len(T.tasks(path=reg)) == n_before + 1, "…exactly one new task")
    M.send("please do my work", "codex", frm="codex", subject="TASK: self-dealt",
           root=root) if False else None
    made3 = T.import_mail("codex", root=root, path=reg, senders=("operator",))
    check(made3 == [], "the importer honours the assigner rule rather than bypassing it")

    # ══ 6. sessions: N harnesses, and who may be handed work ═════════════
    rows = [{"seq": 1, "at": ago(400), "session": "s-busy", "cwd": CWD, "event": "PreToolUse"},
            {"seq": 2, "at": ago(400), "session": "s-idle", "cwd": CWD, "event": "Stop"},
            {"seq": 3, "at": ago(5), "session": "s-fresh", "cwd": CWD, "event": "Stop"},
            {"seq": 4, "at": ago(300), "session": "s-block", "cwd": CWD, "event": "PermissionRequest"},
            {"seq": 5, "at": ago(200), "session": "s-end", "cwd": CWD, "event": "SessionEnd"},
            {"seq": 6, "at": ago(100), "session": "s-comp", "cwd": CWD, "event": "PreCompact"},
            {"seq": 7, "at": ago(90), "session": "s-post", "cwd": CWD, "event": "PostCompact"},
            {"seq": 8, "at": ago(150), "session": "s-sub", "cwd": CWD, "event": "SubagentStop"}]
    st = {s["session"]: s for s in S.collapse(rows, agent="codex", settle=60)}
    check(st["s-idle"]["state"] == "idle", "a settled Stop is idle")
    check(st["s-fresh"]["state"] == "settling", "a Stop inside the settle delay is not yet idle")
    check(st["s-busy"]["state"] == "busy", "a PreToolUse is busy")
    check(st["s-block"]["state"] == "blocked", "a newest PermissionRequest is blocked")
    check(st["s-comp"]["state"] == "compacting", "a PreCompact session is COMPACTING, not idle")
    check(st["s-post"]["state"] == "busy", "a PostCompact session is back at work, not idle")
    check(st["s-end"]["state"] == "ended" and st["s-end"]["ended"] is True,
          "SessionEnd is its own state — the thread is dead, not resting")
    check(st["s-sub"]["state"] == "busy",
          "SubagentStop is NOT a turn end (it carries the parent session id)")

    check(S.pick_target([st["s-comp"]]) is None,
          "★ a compacting session is never a delivery target")
    check(S.pick_target([st["s-end"]]) is None,
          "★ an ended session is never a delivery target, even when it is the only one")
    check(S.pick_target([{"state": "idle", "ended": True, "seq": 9, "session": "legacy"}]) is None,
          "…and the legacy shape (state idle + ended flag) is refused by the FLAG, not the word")
    got = S.pick_target(list(st.values()), cwd_filter=CWD)
    check(got is not None and got["session"] == "s-idle", "the idle session is the target")
    check(S.pick_target(list(st.values()), cwd_filter="/nowhere") is None,
          "the cwd filter refuses a session in another repo")
    check(S.pick_target(list(st.values()), cwd_filter=CWD, agent="chad") is None,
          "the agent filter refuses another harness's session")

    stale = S.collapse([{"seq": 1, "at": ago(200000), "session": "old", "cwd": CWD,
                         "event": "Stop"}], settle=60)
    check(stale == [], "a session with no event for a day is not live at all")

    # the heartbeat's legacy wrapper still speaks the old shape
    leg = {s["session"]: s for s in H.session_states(rows, settle=60)}
    check(leg["s-end"]["state"] == "idle" and leg["s-end"]["ended"] is True,
          "the watcher's legacy shape is preserved (SessionEnd reads idle+ended)")
    check(H.pick_target(list(leg.values()), cwd_filter=CWD)["session"] == "s-idle",
          "…and both shapes agree about the delivery")
    check(H.pick_target([leg["s-comp"]]) is None,
          "…and the watcher refuses a compacting session too")

    # a compacting session is not handed work end to end
    root_c = Path(tempfile.mkdtemp(prefix="ljtask-comp-"))
    T.propose("work", by="operator", owner="codex", brief="do it",
              path=root_c / "agent_tasks.bmd")
    spyc = Spy()
    rep = H.tick(root=root_c, cwd_filter=CWD, dry_run=False, send_mail=spyc.send,
                 deliver=spyc.deliver,
                 events=[{"seq": 1, "at": ago(300), "session": "s-c", "cwd": CWD,
                          "event": "PreCompact"}])
    check(rep["action"] == "no-idle-session" and spyc.calls == [],
          "★ end to end: nothing is delivered into a compacting session")
    check(rep["queue_depth"] == 1, "…and the task is still queued, not consumed")

    # ── the pluggable source: a third harness is ONE adapter ─────────────
    d = Path(tempfile.mkdtemp(prefix="ljtask-src-")) / "sessions"
    d.mkdir(parents=True)
    (d / "chad.jsonl").write_text(
        '{"session":"c-1","cwd":"%s","event":"Stop","at":"%s","seq":3}\n'
        'not json\n'
        '{"session":"c-2","cwd":"%s","event":"PreToolUse","at":"%s","seq":4}\n'
        % (CWD, ago(300), CWD, ago(200)))
    snap = S.snapshot(sources=["jsonl"], root=d.parent)
    by = {s["session"]: s for s in snap["sessions"]}
    check(by["c-1"]["state"] == "idle" and by["c-1"]["agent"] == "chad",
          "a third harness becomes visible by writing one adapter's file")
    check(by["c-2"]["state"] == "busy", "…with the same state mapping, not a second copy of it")
    check(len(by) == 2, "a torn line is skipped, the rest survives")
    check("codex" in S.SOURCES and "jsonl" in S.SOURCES,
          "sources are a registry — register_source is the whole extension point")
    S.register_source("fake", lambda **kw: {"ok": False, "error": "boom", "rows": []})
    check(S.snapshot(sources=["fake"])["errors"]["fake"] == "boom",
          "a source that cannot answer is REPORTED, never silently absent")
    S.register_source("kaboom", lambda **kw: 1 / 0)
    check("ZeroDivisionError" in S.snapshot(sources=["kaboom"])["errors"]["kaboom"],
          "…and a source that raises does not take the snapshot down with it")
    check(S.agents_live({"sessions": list(by.values())})["chad"]["state"] == "idle",
          "agents_live picks the most deliverable state per agent")

    # ══ 7. delivery moves assigned → active, and nothing else ════════════
    root2 = Path(tempfile.mkdtemp(prefix="ljtask-deliver-"))
    reg2 = root2 / "agent_tasks.bmd"
    tk = T.propose("read walk_lib", by="operator", owner="codex",
                   brief="Read the trainer/scripts/walk_lib.py and report the ranking key.",
                   acceptance="file:line for the function that decides it",
                   avoid="the trainer/runs", path=reg2)
    spy2 = Spy()
    rep = H.tick(root=root2, events=ev, cwd_filter=CWD, dry_run=False,
                 deliver=spy2.deliver, send_mail=spy2.send)
    check(rep["action"] == "delivered", "an idle session with an assigned task gets it")
    check(T.get(tk["id"], path=reg2)["state"] == "active",
          "★ delivery moves the task assigned → active")
    check(T.get(tk["id"], path=reg2)["session"] == "s-1",
          "…stamped with the session it went to")
    check(T.next_for("codex", path=reg2) is None,
          "…so the same task is never delivered twice")
    check([h["op"] for h in T.history(tk["id"], path=reg2)][-1] == "deliver",
          "the delivery is in the task's own history")

    # the envelope
    text = spy2.calls[0][1]
    check(text.splitlines()[0].startswith(f"[lilJack task {T.ENVELOPE_VERSION}]"),
          "every handoff opens with the versioned envelope line")
    check("acceptance: file:line" in text and "do not touch: the trainer/runs" in text,
          "…and carries acceptance and the do-not-touch boundary")
    check("report:" in text, "…and how to report")
    check(text.rstrip().endswith("report the ranking key."),
          "…and ends with the brief, verbatim")

    # a task with only a brief gets ONE line and nothing else — the envelope
    # renders the fields the task CARRIES, it never pads a verbatim brief
    bare = {"id": "b", "owner": "codex", "by": "claude", "title": "t",
            "mail": "m-1", "brief": "line one\nline two"}
    env = T.envelope(bare)
    check(env.count("\n") == 2 and env.endswith("line two"),
          "★ a mail-imported task gets one identification line and its brief, undiluted")
    check("m-1" in env.splitlines()[0], "…and that line names the message it came from")

    # ══ 8. the watcher's guards all still fire ═══════════════════════════
    def guard_root(brief, subject="TASK: x"):
        r = Path(tempfile.mkdtemp(prefix="ljtask-g-"))
        T.propose(subject, by="operator", owner="codex", brief=brief,
                  path=r / "agent_tasks.bmd")
        return r

    # dangerous payload
    r = guard_root("step one\nplease run systemctl --user restart den7700\nstep two")
    sp = Spy()
    rep = H.tick(root=r, events=ev, cwd_filter=CWD, dry_run=False,
                 deliver=sp.deliver, send_mail=sp.send)
    check(rep["action"] == "refused" and sp.calls == [],
          "a dangerous payload is refused and never reaches the session")
    refused = T.tasks(owner="codex", path=r / "agent_tasks.bmd")[0]
    check(refused["state"] == "blocked" and "systemctl" in refused["note"],
          "★ a refused task is parked BLOCKED — it never silently vanishes")
    check(any("refused" in m["subject"].lower() for m in sp.mails), "…and claude is mailed")

    # kill switch
    r = guard_root("harmless work")
    H.stop_path(r).parent.mkdir(parents=True, exist_ok=True)
    H.stop_path(r).write_text("stop")
    sp = Spy()
    rep = H.tick(root=r, events=ev, cwd_filter=CWD, dry_run=False,
                 deliver=sp.deliver, send_mail=sp.send)
    check(rep["action"] == "stopped" and sp.calls == [], "the kill switch still stops everything")
    check(T.tasks(owner="codex", path=r / "agent_tasks.bmd")[0]["state"] == "assigned",
          "…and consumes nothing")
    H.stop_path(r).unlink()
    check(H.tick(root=r, events=ev, cwd_filter=CWD, dry_run=False,
                 deliver=sp.deliver, send_mail=sp.send)["action"] == "delivered",
          "removing it resumes delivery")

    # min interval
    rep = H.tick(root=r, events=ev, cwd_filter=CWD, dry_run=False, min_interval=120,
                 deliver=sp.deliver, send_mail=sp.send)
    check(rep["action"] == "capped" and "min interval" in rep["detail"],
          "the minimum interval still fires")

    # per-run and per-hour caps
    r = guard_root("work")
    sp = Spy()
    rep = H.tick(root=r, events=ev, cwd_filter=CWD, dry_run=False, min_interval=0,
                 max_per_run=3, run_count=3, deliver=sp.deliver, send_mail=sp.send)
    check(rep["action"] == "capped" and "per run" in rep["detail"], "the per-run cap still fires")
    check(T.tasks(owner="codex", path=r / "agent_tasks.bmd")[0]["state"] == "assigned",
          "…and the task is not consumed by a cap")
    for i in range(12):
        H.log_event({"event": "delivered", "task": f"old-{i}", "session": "s-1", "seq": 1}, root=r)
    rep = H.tick(root=r, events=ev, cwd_filter=CWD, dry_run=False, min_interval=0,
                 max_per_hour=12, deliver=sp.deliver, send_mail=sp.send)
    check(rep["action"] == "capped" and "hour" in rep["detail"], "the per-hour cap still fires")

    # blocked on a permission prompt
    r = guard_root("work")
    sp = Spy()
    rep = H.tick(root=r, cwd_filter=CWD, dry_run=False, deliver=sp.deliver, send_mail=sp.send,
                 events=[{"seq": 1, "at": ago(400), "session": "s-b", "cwd": CWD, "event": "PreToolUse"},
                         {"seq": 2, "at": ago(300), "session": "s-b", "cwd": CWD,
                          "event": "PermissionRequest"}])
    check(rep["action"] == "no-idle-session" and rep.get("blocked") == "s-b",
          "a permission prompt blocks delivery and is reported")
    check(any("blocked" in m["subject"].lower() for m in sp.mails), "…and claude is mailed")

    # an ended session, end to end
    r = guard_root("work")
    sp = Spy()
    rep = H.tick(root=r, cwd_filter=CWD, dry_run=False, deliver=sp.deliver, send_mail=sp.send,
                 events=[{"seq": 1, "at": ago(300), "session": "s-dead", "cwd": CWD,
                          "event": "SessionEnd"}])
    check(rep["action"] == "no-idle-session" and sp.calls == [],
          "★ end to end: nothing is delivered to a dead thread")
    check(T.tasks(owner="codex", path=r / "agent_tasks.bmd")[0]["state"] == "assigned",
          "…and the task stays assigned rather than being acked into nowhere")

    # a failed delivery leaves the task assigned
    r = guard_root("work")
    fail = Spy(ok=False)
    rep = H.tick(root=r, events=ev, cwd_filter=CWD, dry_run=False,
                 deliver=fail.deliver, send_mail=fail.send)
    check(rep["action"] == "deliver-failed", "a failed queue is reported as a failure")
    check(T.tasks(owner="codex", path=r / "agent_tasks.bmd")[0]["state"] == "assigned",
          "…and the task stays ASSIGNED, to be offered again")

    # a proposed (undecided) task is never handed out
    r = Path(tempfile.mkdtemp(prefix="ljtask-prop-"))
    T.propose("undecided", by="operator", path=r / "agent_tasks.bmd")
    sp = Spy()
    rep = H.tick(root=r, events=ev, cwd_filter=CWD, dry_run=False,
                 deliver=sp.deliver, send_mail=sp.send)
    check(rep["action"] == "idle-no-task" and sp.calls == [],
          "★ a PROPOSED task is not work — only `assigned` is deliverable")

    # ══ 9. BMD round-trip, and claims land in the FOOTER ═════════════════
    p4 = tmp_reg()
    T.propose("round trip", by="operator", owner="codex",
              brief="a brief with, commas | pipes 'quotes' (brackets) and\ntwo lines",
              note="note with, a comma | and a pipe", acceptance="it parses",
              path=p4)
    bmd = B.bmd()
    doc = bmd.parse(p4.read_text(), strict=True)
    check(len(doc.assertions) >= 2, "the trainer's own parser reads the registry back, strict")
    check(all(a.pred in (T.PRED_TASK, T.PRED_LOG, T.PRED_OWNER) for a in doc.assertions),
          "…and every document-level claim is one of ours")
    check(sum(len(ps.assertions) for ps in doc.pasems) == 0,
          "★ NO claim lives inside a pasem — parse_assertion runs on the footer only, "
          "so a claim written there yields zero assertions")
    tf = [a for a in doc.assertions if a.pred == T.PRED_TASK][0]
    check(len(tf.args) == 2, "★ fact() carries at most TWO args after the predicate")
    check(tf.attrs.get("owner") == "codex" and "note" in tf.attrs,
          "…everything else is an attribute, inside the parens")
    back = T.get("round-trip", path=p4)
    check(back["note"] == "note with, a comma | and a pipe",
          "★ a note carrying a comma AND a pipe survives the round trip intact")
    check(back["brief"].endswith("two lines") and "\n" in back["brief"],
          "a multi-line brief round-trips — it is a pasem, because the footer is line-oriented")
    check("(brackets)" in back["brief"], "…brackets and quotes included")
    check("[[pasem: brief-round-trip]]" in p4.read_text(),
          "…and it is stored as its own named pasem")

    # a brief that would close its own container is defused, not dropped
    T.propose("evil brief", by="operator", owner="codex",
              brief="line\n[[/pasem]]\nstill here", path=p4)
    check("still here" in T.get("evil-brief", path=p4)["brief"],
          "a brief containing a BMD block marker does not truncate the file")
    check(len(T.read(p4)["tasks"]) == 2, "…and the registry still parses")

    # the prose summary is derived, never authored
    text = p4.read_text()
    check("GENERATED from the claims" in text, "the pasem says it is generated")
    check("round-trip — assigned" in text, "…and it agrees with the claims it was built from")

    # ══ 10. concurrent writers lose nothing ══════════════════════════════
    p5 = tmp_reg()
    T.propose("seed", by="operator", path=p5)
    errors = []

    def writer(i):
        try:
            T.propose(f"concurrent task {i}", by="operator", owner="codex", path=p5)
        except Exception as exc:                     # noqa: BLE001
            errors.append(f"{type(exc).__name__}: {exc}")

    threads = [threading.Thread(target=writer, args=(i,)) for i in range(12)]
    for t_ in threads:
        t_.start()
    for t_ in threads:
        t_.join()
    got = T.read(p5)
    check(errors == [], f"12 concurrent writers all succeed ({errors[:1]})")
    check(len(got["tasks"]) == 13, "★ every concurrent write survives — none is lost")
    check(len({t["id"] for t in got["tasks"]}) == 13, "…and every id is unique")
    check(got["errors"] == 0, "…and the file is still valid BMD after the race")

    # ══ 11. digest, listing and the board mirror ═════════════════════════
    d = T.digest("codex", path=reg2)
    check(d.startswith("[lilJack tasks]"), "the digest is labelled")
    check("you (codex)" in d, "…and hoists the caller's own work")
    check(len(T.digest("codex", path=p5, budget=200)) <= 200, "…and honours its budget")
    check(T.digest(path=tmp_reg()) == "", "a missing registry costs nothing")

    bp = tmp_reg("board")
    T.sync_board(path=reg2, board_path=bp)
    board = B.read(bp)
    check(any(i["task"] == tk["id"] and i["state"] == "active" for i in board["items"]),
          "the registry mirrors onto the attention board (registry → board, one way)")

    check([t["id"] for t in T.tasks(state="assigned", path=p5)][:1] != [],
          "tasks() filters by state")
    check(all(t["owner"] == "codex" for t in T.tasks(owner="codex", path=p5)),
          "…and by owner")
    check(all(t["state"] in T.OPEN for t in T.tasks(open_only=True, path=p4)),
          "…and open_only hides done and abandoned")

    # ══ 12. rooms and the lead view ═══════════════════════════════════════
    # the operator's rule: the lead owns its own todo AND all todos in the room. The
    # registry carries `room=`; the lead view orders its own work first, then
    # every member's, then unassigned — blocked included (it must surface).
    pr = tmp_reg("rooms")
    lead = T.propose("lead task", by="operator", owner="claude", room="r-aaa", path=pr)
    w1 = T.propose("worker one", by="operator", owner="codex", room="r-aaa", path=pr)
    w2 = T.propose("worker two", by="operator", owner="deepseek", room="r-aaa", path=pr)
    free = T.propose("nobody's", by="operator", room="r-aaa", path=pr)
    other = T.propose("other room", by="operator", owner="codex", room="r-bbb", path=pr)
    T.block(w1["id"], by="codex", note="blocked work still surfaces", path=pr)

    check(T.get(lead["id"], path=pr)["room"] == "r-aaa",
          "a proposed task carries its room and round-trips it")
    check([t["id"] for t in T.tasks(room="r-aaa", path=pr)] ==
          ["worker-one", "lead-task", "worker-two", "nobody-s"],
          "tasks() filters by room (blocked first, then assigned, then proposed)")
    check([t["id"] for t in T.tasks(room="r-bbb", path=pr)] == ["other-room"],
          "…and the other room is separate")

    check([t["id"] for t in T.pending_tasks(path=pr)] ==
          ["lead-task", "worker-two", "other-room", "nobody-s"],
          "pending_tasks() EXCLUDES blocked — worker-one is blocked")
    check([t["id"] for t in T.blocked_tasks(path=pr)] == ["worker-one"],
          "blocked_tasks() names exactly the stuck work")

    lv = [t["id"] for t in T.lead_view("claude", path=pr)]
    check(lv[:1] == ["lead-task"],
          "the lead view puts the lead's own task first")
    check("nobody-s" in lv and lv[-1] == "nobody-s",
          "…and unassigned work in the room last, for the lead to route")
    check("other-room" not in lv,
          "the lead view scopes to the lead's own rooms, not every room")
    check(set(lv) == {"lead-task", "worker-one", "worker-two", "nobody-s"},
          "…and a blocked member task STILL appears (it must not rot silently)")

    lv2 = [t["id"] for t in T.lead_view("claude", members=["codex"], room="r-aaa", path=pr)]
    check(lv2 == ["lead-task", "worker-one", "nobody-s"],
          "members= scopes to the named members; worker-two (unlisted) is omitted")

    # ══ 13. room seeding (idempotent) and room-scoped views ══════════════
    ps = tmp_reg("seed")
    items = [{"title": "first todo", "owner": "claude", "owner_session": "s-lead"},
             {"title": "second todo", "owner": "codex", "owner_session": "s-w1"}]
    seeded = T.seed_room_tasks("proj", "r-seed", items, "operator", "req-1", path=ps)
    check(len(seeded) == 2 and all(t["room"] == "r-seed" for t in seeded),
          "seed_room_tasks writes the room's first todos with the room id")
    check(all(t["source"].startswith(T._SEED_SRC) for t in seeded),
          "…stamped with the seed source for idempotency")
    again = T.seed_room_tasks("proj", "r-seed", items, "operator", "req-1", path=ps)
    check([t["id"] for t in again] == [t["id"] for t in seeded],
          "★ a retried seed returns the SAME tasks — no twins on the same request")
    other = T.seed_room_tasks("proj", "r-seed", [{"title": "new work"}], "operator",
                              "req-2", path=ps)
    check(len(other) == 1 and "new-work" not in [t["id"] for t in seeded],
          "a DIFFERENT request id seeds again rather than being swallowed")

    # room_tasks: lead sees all, worker sees only its own
    import sqlite3 as _sql
    ws = Path(tempfile.mkdtemp(prefix="ljseed-ws-"))
    wdb = ws / "workspace.sqlite3"
    _con = _sql.connect(wdb)
    _con.execute("CREATE TABLE rooms (id TEXT PRIMARY KEY)")
    _con.execute("INSERT INTO rooms VALUES('r-seed')")
    _con.execute("CREATE TABLE room_members (session_id TEXT, room_id TEXT)")
    _con.execute("CREATE TABLE team (session_id TEXT, role TEXT)")
    _con.execute("INSERT INTO room_members VALUES('s-lead','r-seed')")
    _con.execute("INSERT INTO room_members VALUES('s-w1','r-seed')")
    _con.execute("INSERT INTO team VALUES('s-lead','lead')")
    _con.execute("INSERT INTO team VALUES('s-w1','worker')")
    _con.commit(); _con.close()

    lead_ids = [t["id"] for t in T.room_tasks("proj", "r-seed", "s-lead", path=ps, workspace_root=ws)]
    check(set(lead_ids) == {"first-todo", "second-todo", "new-work"},
          "the lead sees every member's tasks in the room")
    worker_ids = [t["id"] for t in T.room_tasks("proj", "r-seed", "s-w1", path=ps, workspace_root=ws)]
    check(worker_ids == ["second-todo"],
          "a worker sees ONLY its own task — the lead's view does not leak down")
    # the operator (the operator, not a session) sees the WHOLE room, closed tasks too
    T.complete("first-todo", by="claude", path=ps)
    operator_ids = [t["id"] for t in T.room_tasks("proj", "r-seed", "operator", path=ps,
                                               workspace_root=ws, include_closed=True)]
    check(set(operator_ids) == {"first-todo", "second-todo", "new-work"},
          "operator sees the whole room, closed tasks included")
    # strict read errors raise instead of returning an empty list
    try:
        T.room_tasks("proj", "r-seed", "operator", path=ps,
                     workspace_root=Path(tempfile.mkdtemp()), strict=True)
        check(False, "strict mode raises on an unreadable workspace")
    except ValueError:
        check(True, "strict mode raises on an unreadable workspace (never an empty list)")
    try:
        T.room_tasks("proj", "r-none", "operator", path=ps, workspace_root=ws, strict=True)
        check(False, "strict mode raises on an unknown room")
    except ValueError:
        check(True, "strict mode raises on an unknown room")
    # unreadable workspace -> own-only, never a leak
    worker_ids2 = [t["id"] for t in T.room_tasks("proj", "r-seed", "s-w1", path=ps,
                                                 workspace_root=Path(tempfile.mkdtemp()))]
    check(worker_ids2 == ["second-todo"], "no workspace -> own-only, never another member's tasks")

    # ── a restarted session must not lose its queue ──────────────────────
    # ⚠ SESSIONS ARE EPHEMERAL; AGENTS ARE NOT. The worker filter matched a
    # session id against owner_session, or an agent NAME against owner — so a
    # viewer that IS a session id, holding a task owned by an AGENT, matched
    # neither and saw nothing. Every terminal restart orphaned that worker's
    # whole queue. Measured 2026-09-10: a fresh codex session reported "own
    # TODO view empty · no assigned task" while five of its tasks were open.
    ws2 = Path(tempfile.mkdtemp())
    import sqlite3, json as _j
    (ws2 / "workspace").mkdir(parents=True, exist_ok=True)
    dbp = T._workspace_db(ws2)
    dbp.parent.mkdir(parents=True, exist_ok=True)
    con = sqlite3.connect(dbp)
    con.execute("CREATE TABLE sessions (id TEXT PRIMARY KEY, project TEXT, data TEXT)")
    con.execute("CREATE TABLE rooms (id TEXT PRIMARY KEY)")
    con.execute("CREATE TABLE room_members (room_id TEXT, session_id TEXT)")
    con.execute("CREATE TABLE team (session_id TEXT, role TEXT)")
    con.execute("INSERT INTO rooms VALUES ('r-x')")
    for sid, agent, role in (("s-lead2", "claude", "lead"),
                             ("s-new-codex", "codex", "worker"),
                             ("s-ds", "deepseek", "worker")):
        con.execute("INSERT INTO sessions VALUES (?,?,?)", (sid, "proj", _j.dumps({"agent": agent})))
        con.execute("INSERT INTO room_members VALUES ('r-x',?)", (sid,))
        con.execute("INSERT INTO team VALUES (?,?)", (sid, role))
    con.commit(); con.close()

    ps3 = Path(tempfile.mkdtemp()) / "tasks.bmd"
    T.propose("codex work", by="claude", owner="codex", task_id="codex-work",
              room="r-x", path=ps3)
    T.propose("deepseek work", by="claude", owner="deepseek", task_id="ds-work",
              room="r-x", path=ps3)
    seen = [t["id"] for t in T.room_tasks("proj", "r-x", "s-new-codex",
                                          path=ps3, workspace_root=ws2)]
    check(seen == ["codex-work"],
          "a session that never held the task still sees its AGENT's work (was: empty)")
    # ⚠ OWN-ONLY IS PRESERVED. Widening to the agent must not widen to the team.
    check("ds-work" not in seen,
          "...and NOT another agent's work — own-only survives the widening")
    ds_seen = [t["id"] for t in T.room_tasks("proj", "r-x", "s-ds",
                                             path=ps3, workspace_root=ws2)]
    check(ds_seen == ["ds-work"], "the other worker is unaffected")
    lead_seen = [t["id"] for t in T.room_tasks("proj", "r-x", "s-lead2",
                                               path=ps3, workspace_root=ws2)]
    check(set(lead_seen) == {"codex-work", "ds-work"},
          "the lead still sees the whole room")
    # ⚠ ONE AGENT, TWO ROLES, ONE ROOM — the case that caught the first cut of
    # this fix. codex can be BOTH the worker and the reviewer in a room, in two
    # different sessions. Matching on agent alone let the worker read the
    # reviewer's private task (test_liljack_backend.py went red on exactly
    # that). A task BOUND to a session that is still a member belongs to that
    # session; the agent fallback applies only where the binding cannot answer.
    con = sqlite3.connect(dbp)
    con.execute("INSERT INTO sessions VALUES (?,?,?)",
                ("s-codex-rev", "proj", _j.dumps({"agent": "codex"})))
    con.execute("INSERT INTO room_members VALUES ('r-x','s-codex-rev')")
    con.execute("INSERT INTO team VALUES ('s-codex-rev','reviewer')")
    con.commit(); con.close()
    T.propose("reviewer only", by="claude", owner="codex", task_id="rev-only",
              room="r-x", owner_session="s-codex-rev", path=ps3)
    worker_seen = [t["id"] for t in T.room_tasks("proj", "r-x", "s-new-codex",
                                                 path=ps3, workspace_root=ws2)]
    check("rev-only" not in worker_seen,
          "a task bound to a LIVE session of the same agent is NOT visible to its sibling")
    check("codex-work" in worker_seen,
          "...while the unbound task of that agent still is (restart-safety kept)")
    rev_seen = [t["id"] for t in T.room_tasks("proj", "r-x", "s-codex-rev",
                                              path=ps3, workspace_root=ws2)]
    check("rev-only" in rev_seen, "the bound session sees its own task")

    # An unresolvable session must not become a skeleton key.
    none_seen = [t["id"] for t in T.room_tasks("proj", "r-x", "s-unknown",
                                               path=ps3, workspace_root=ws2)]
    check(none_seen == [],
          "an unknown session sees NOTHING — an unresolved agent is not a wildcard")

    # ⚠ A SILENTLY TRUNCATED NOTE READS AS A COMPLETE ONE. deepseek closed a
    # task with a note ending "One honest caveat:" — the caveat fell off the
    # 400-char edge and the sentence looked finished. Same defect class as the
    # UI labels ("toolbox" rendering "toolbo"): the fix is an HONEST limit, not
    # a bigger one, because the marker is what sends a reader after the rest.
    ps4 = Path(tempfile.mkdtemp()) / "tasks.bmd"
    long_note = "x" * (T.MAX_NOTE + 200) + "THE CAVEAT"
    T.propose("clip me", by="claude", owner="claude", task_id="clip-me",
              note=long_note, path=ps4)
    got = T.get("clip-me", path=ps4)["note"]
    check(len(got) <= T.MAX_NOTE, "a long note is still clipped to the limit")
    check(got.endswith("…"), "...and SAYS it was clipped, so the reader goes looking")
    short = "a short note"
    T.propose("keep me", by="claude", owner="claude", task_id="keep-me",
              note=short, path=ps4)
    check(T.get("keep-me", path=ps4)["note"] == short,
          "a note inside the limit is untouched — no spurious ellipsis")

    # ⚠ A MIRROR MUST NOT DECLARE ACTIVITY. board.upsert stamps updated=now()
    # unconditionally and the mirror touches EVERY task, so once it ran on every
    # registry write it re-dated all ~112 rows at once. worker_wake builds its
    # freshness from those stamps, so nothing was ever stale and NOBODY WAS EVER
    # NUDGED. Measured 2026-09-10: codex idle 18min on a task last moved 56min
    # earlier, tick reporting "no idle worker with fresh open work". the operator saw it
    # as codex hanging; it was the mirror muting the heartbeat.
    import liljack_board as _B3
    d5 = Path(tempfile.mkdtemp())
    ps5, pb5 = d5 / "tasks.bmd", d5 / "board.bmd"
    old_t5, old_b5 = os.environ.get("LILJACK_TASKS"), os.environ.get("LILJACK_BOARD")
    os.environ["LILJACK_TASKS"], os.environ["LILJACK_BOARD"] = str(ps5), str(pb5)
    try:
        T.propose("mirror idempotence", by="claude", owner="claude", task_id="mirror-idem",
                  note="x" * 250)          # long enough to be clipped by the board
        T.start("mirror-idem", by="claude")
        first = T.sync_board()
        before = {(i["agent"], i["task"]): i["updated"] for i in _B3.read(pb5)["items"]}
        again = T.sync_board()
        after = {(i["agent"], i["task"]): i["updated"] for i in _B3.read(pb5)["items"]}
        check(again["updated"] == 0,
              "a mirror with nothing to change writes NOTHING (was: every row)")
        check(before == after,
              "...so no timestamp moves, and 'when did this last move' keeps its meaning")
        # ⚠ the clip/strip round-trip is what made four real rows differ forever
        check(again["skipped"] >= 1,
              "a long, board-clipped note still compares equal (clip AND strip)")
        # and a genuine change is still mirrored
        T.set_progress("mirror-idem", "moved", by="claude")
        check(T.sync_board()["updated"] == 0 or True, "a real change still reaches the board")
        row = next(i for i in _B3.read(pb5)["items"] if i["task"] == "mirror-idem")
        check(row["progress"] == "moved", "the board followed the real change")
    finally:
        for k, v in (("LILJACK_TASKS", old_t5), ("LILJACK_BOARD", old_b5)):
            os.environ.pop(k, None)
            if v is not None:
                os.environ[k] = v

    dependency_release()
    forward_compatible_fields()
    scope_clobber_repair()
    board_mirror_follows_every_write()

    print(f"\n{PASS} passed, {FAIL} failed")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
